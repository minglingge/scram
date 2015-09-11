/*
 * Copyright (C) 2014-2015 Olzhas Rakhimov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// @file initializer.cc
/// Implementation of input file processing into analysis constructs.

#include "initializer.h"

#include <fstream>
#include <unordered_map>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

#include "ccf_group.h"
#include "cycle.h"
#include "element.h"
#include "env.h"
#include "error.h"
#include "expression.h"
#include "fault_tree.h"
#include "logger.h"

namespace fs = boost::filesystem;

namespace scram {

const std::map<std::string, Units> Initializer::kUnits_ =
    {{"bool", kBool}, {"int", kInt}, {"float", kFloat}, {"hours", kHours},
     {"hours-1", kInverseHours}, {"years", kYears}, {"years-1", kInverseYears},
     {"fit", kFit}, {"demands", kDemands}};

const char* const Initializer::kUnitToString_[] = {"unitless", "bool", "int",
                                                   "float", "hours", "hours-1",
                                                   "years", "years-1", "fit",
                                                   "demands"};

std::stringstream Initializer::schema_;

Initializer::Initializer(const Settings& settings) {
  settings_ = settings;
  mission_time_ = std::shared_ptr<MissionTime>(new MissionTime());
  mission_time_->mission_time(settings_.mission_time());
  if (schema_.str().empty()) {
    std::string schema_path = Env::input_schema();
    std::ifstream schema_stream(schema_path.c_str());
    schema_ << schema_stream.rdbuf();
    schema_stream.close();
  }
}

void Initializer::ProcessInputFiles(const std::vector<std::string>& xml_files) {
  CLOCK(input_time);
  LOG(DEBUG1) << "Processing input files";
  std::vector<std::string>::const_iterator it;
  try {
    for (it = xml_files.begin(); it != xml_files.end(); ++it) {
      Initializer::ProcessInputFile(*it);
    }
  } catch (ValidationError& err) {
    err.msg("In file '" + *it + "', " + err.msg());
    throw err;
  }
  CLOCK(def_time);
  Initializer::ProcessTbdElements();
  LOG(DEBUG2) << "Element definition time " << DUR(def_time);
  LOG(DEBUG1) << "Input files are processed in " << DUR(input_time);

  CLOCK(valid_time);
  LOG(DEBUG1) << "Validating the input files";
  // Check if the initialization is successful.
  Initializer::ValidateInitialization();
  LOG(DEBUG1) << "Validation is finished in " << DUR(valid_time);

  CLOCK(setup_time);
  LOG(DEBUG1) << "Setting up for the analysis";
  // Perform setup for analysis using configurations from the input files.
  Initializer::SetupForAnalysis();
  LOG(DEBUG1) << "Setup time " << DUR(setup_time);
}

void Initializer::ProcessInputFile(const std::string& xml_file) {
  std::ifstream file_stream(xml_file.c_str());
  if (!file_stream) {
    throw IOError("File '" + xml_file + "' could not be loaded.");
  }

  // Collection of input file locations in canonical path.
  std::set<std::string> input_paths;
  fs::path file_path = fs::canonical(xml_file);
  if (input_paths.count(file_path.native())) {
    throw ValidationError("Trying to pass the same file twice: " +
                          file_path.native());
  }
  input_paths.insert(file_path.native());

  std::stringstream stream;
  stream << file_stream.rdbuf();
  file_stream.close();

  XMLParser* parser(new XMLParser(stream));
  parsers_.emplace_back(parser);
  parser->Validate(schema_);

  const xmlpp::Document* doc = parser->Document();
  const xmlpp::Node* root = doc->get_root_node();
  assert(root->get_name() == "opsa-mef");
  doc_to_file_.insert(std::make_pair(root, xml_file));  // Save for later.

  if (!model_) {  // Create only one model for multiple files.
    const xmlpp::Element* root_element =
        static_cast<const xmlpp::Element*>(root);
    std::string model_name = root_element->get_attribute_value("name");
    boost::trim(model_name);  // The name may be empty. It is optional.
    model_ = ModelPtr(new Model(model_name));
    Initializer::AttachLabelAndAttributes(root_element, model_.get());
  }

  xmlpp::NodeSet::iterator it_ch;  // Iterator for all children.

  xmlpp::NodeSet fault_trees = root->find("./define-fault-tree");
  for (it_ch = fault_trees.begin(); it_ch != fault_trees.end(); ++it_ch) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it_ch);
    Initializer::DefineFaultTree(element);
  }

  xmlpp::NodeSet ccf_groups = root->find("./define-CCF-group");
  for (it_ch = ccf_groups.begin(); it_ch != ccf_groups.end(); ++it_ch) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it_ch);
    Initializer::RegisterCcfGroup(element);
  }

  xmlpp::NodeSet model_data = root->find("./model-data");
  for (it_ch = model_data.begin(); it_ch != model_data.end(); ++it_ch) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it_ch);
    Initializer::ProcessModelData(element);
  }
}

void Initializer::ProcessTbdElements() {
  // This element helps report errors.
  const xmlpp::Element* el_def;  // XML element with the definition.
  try {
    std::vector<std::pair<ParameterPtr,
        const xmlpp::Element*> >::iterator it_p;
    for (it_p = tbd_.parameters.begin(); it_p != tbd_.parameters.end();
         ++it_p) {
      el_def = it_p->second;
      Initializer::DefineParameter(el_def, it_p->first);
    }
    std::vector<std::pair<BasicEventPtr,
        const xmlpp::Element*> >::iterator it_b;
    for (it_b = tbd_.basic_events.begin(); it_b != tbd_.basic_events.end();
         ++it_b) {
      el_def = it_b->second;
      Initializer::DefineBasicEvent(el_def, it_b->first);
    }
    std::vector<std::pair<GatePtr,
        const xmlpp::Element*> >::iterator it_g;
    for (it_g = tbd_.gates.begin(); it_g != tbd_.gates.end(); ++it_g) {
      el_def = it_g->second;
      Initializer::DefineGate(el_def, it_g->first);
    }
    std::vector<std::pair<CcfGroupPtr,
        const xmlpp::Element*> >::iterator it_c;
    for (it_c = tbd_.ccf_groups.begin(); it_c != tbd_.ccf_groups.end();
         ++it_c) {
      el_def = it_c->second;
      Initializer::DefineCcfGroup(el_def, it_c->first);
    }
  } catch (ValidationError& err) {
    const xmlpp::Node* root = el_def->find("/opsa-mef")[0];
    err.msg("In file '" + doc_to_file_.find(root)->second + "', " + err.msg());
    throw err;
  }
}

void Initializer::AttachLabelAndAttributes(const xmlpp::Element* element_node,
                                           Element* element) {
  xmlpp::NodeSet labels = element_node->find("./label");
  if (!labels.empty()) {
    assert(labels.size() == 1);
    const xmlpp::Element* label =
        static_cast<const xmlpp::Element*>(labels.front());
    const xmlpp::TextNode* text = label->get_child_text();
    assert(text);
    element->label(text->get_content());
  }

  xmlpp::NodeSet attributes = element_node->find("./attributes");
  if (!attributes.empty()) {
    assert(attributes.size() == 1);  // Only one big element 'attributes'.
    const xmlpp::Element* attributes_element =
        static_cast<const xmlpp::Element*>(attributes.front());
    xmlpp::NodeSet attribute_list = attributes_element->find("./attribute");
    xmlpp::NodeSet::iterator it;
    for (it = attribute_list.begin(); it != attribute_list.end(); ++it) {
      const xmlpp::Element* attribute = static_cast<const xmlpp::Element*>(*it);
      Attribute attr;
      attr.name = attribute->get_attribute_value("name");
      boost::trim(attr.name);
      attr.value = attribute->get_attribute_value("value");
      boost::trim(attr.value);
      attr.type = attribute->get_attribute_value("type");
      boost::trim(attr.type);
      element->AddAttribute(attr);
    }
  }
}

void Initializer::DefineFaultTree(const xmlpp::Element* ft_node) {
  std::string name = ft_node->get_attribute_value("name");
  boost::trim(name);
  assert(!name.empty());
  FaultTreePtr fault_tree(new FaultTree(name));
  Initializer::RegisterFaultTreeData(ft_node, name, fault_tree.get());
  try {
    model_->AddFaultTree(std::move(fault_tree));
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << ft_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
}

std::unique_ptr<Component> Initializer::DefineComponent(
    const xmlpp::Element* component_node,
    const std::string& base_path,
    bool public_container) {
  std::string name = component_node->get_attribute_value("name");
  boost::trim(name);
  assert(!name.empty());
  std::string role = component_node->get_attribute_value("role");
  boost::trim(role);
  bool component_role = public_container;  // Inherited role by default.
  // Overwrite the role explicitly.
  if (role != "") component_role = role == "public" ? true : false;
  ComponentPtr component(new Component(name, base_path, component_role));
  Initializer::RegisterFaultTreeData(component_node, base_path + "." + name,
                                     component.get());
  return component;
}

void Initializer::RegisterFaultTreeData(const xmlpp::Element* ft_node,
                                        const std::string& base_path,
                                        Component* component) {
  Initializer::AttachLabelAndAttributes(ft_node, component);

  xmlpp::NodeSet house_events = ft_node->find("./define-house-event");
  xmlpp::NodeSet basic_events = ft_node->find("./define-basic-event");
  xmlpp::NodeSet parameters = ft_node->find("./define-parameter");
  xmlpp::NodeSet gates = ft_node->find("./define-gate");
  xmlpp::NodeSet ccf_groups = ft_node->find("./define-CCF-group");
  xmlpp::NodeSet components = ft_node->find("./define-component");

  xmlpp::NodeSet::iterator it;
  for (it = house_events.begin(); it != house_events.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    component->AddHouseEvent(
        Initializer::DefineHouseEvent(element, base_path,
                                      component->is_public()));
  }
  CLOCK(basic_time);
  for (it = basic_events.begin(); it != basic_events.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    component->AddBasicEvent(
        Initializer::RegisterBasicEvent(element, base_path,
                                        component->is_public()));
  }
  LOG(DEBUG2) << "Basic event registration time " << DUR(basic_time);
  for (it = parameters.begin(); it != parameters.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    component->AddParameter(
        Initializer::RegisterParameter(element, base_path,
                                       component->is_public()));
  }
  CLOCK(gate_time);
  for (it = gates.begin(); it != gates.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    component->AddGate(Initializer::RegisterGate(element, base_path,
                                                 component->is_public()));
  }
  LOG(DEBUG2) << "Gate registration time " << DUR(gate_time);
  for (it = ccf_groups.begin(); it != ccf_groups.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    component->AddCcfGroup(
        Initializer::RegisterCcfGroup(element, base_path,
                                      component->is_public()));
  }
  for (it = components.begin(); it != components.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    ComponentPtr sub = Initializer::DefineComponent(element, base_path,
                                                    component->is_public());
    try {
      component->AddComponent(std::move(sub));
    } catch (ValidationError& err) {
      std::stringstream msg;
      msg << "Line " << element->get_line() << ":\n";
      err.msg(msg.str() + err.msg());
      throw err;
    }
  }
}

void Initializer::ProcessModelData(const xmlpp::Element* model_data) {
  xmlpp::NodeSet house_events = model_data->find("./define-house-event");
  xmlpp::NodeSet basic_events = model_data->find("./define-basic-event");
  xmlpp::NodeSet parameters = model_data->find("./define-parameter");

  xmlpp::NodeSet::iterator it;

  for (it = house_events.begin(); it != house_events.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    Initializer::DefineHouseEvent(element);
  }
  CLOCK(basic_time);
  for (it = basic_events.begin(); it != basic_events.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    Initializer::RegisterBasicEvent(element);
  }
  LOG(DEBUG2) << "Basic event registration time " << DUR(basic_time);
  for (it = parameters.begin(); it != parameters.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    Initializer::RegisterParameter(element);
  }
}

std::shared_ptr<Gate> Initializer::RegisterGate(const xmlpp::Element* gate_node,
                                                const std::string& base_path,
                                                bool public_container) {
  std::string name = gate_node->get_attribute_value("name");
  boost::trim(name);
  std::string role = gate_node->get_attribute_value("role");
  boost::trim(role);
  bool gate_role = public_container;  // Inherited role by default.
  if (role != "") gate_role = role == "public" ? true : false;
  GatePtr gate(new Gate(name, base_path, gate_role));
  try {
    model_->AddGate(gate);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << gate_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
  tbd_.gates.push_back(std::make_pair(gate, gate_node));
  Initializer::AttachLabelAndAttributes(gate_node, gate.get());
  return gate;
}

void Initializer::DefineGate(const xmlpp::Element* gate_node,
                             const GatePtr& gate) {
  xmlpp::NodeSet formulas =
      gate_node->find("./*[name() != 'attributes' and name() != 'label']");
  // Assumes that there are no attributes and labels.
  assert(formulas.size() == 1);
  const xmlpp::Element* formula_node =
      static_cast<const xmlpp::Element*>(formulas.front());
  gate->formula(Initializer::GetFormula(formula_node, gate->base_path()));
  try {
    gate->Validate();
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << gate_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
}

std::unique_ptr<Formula> Initializer::GetFormula(
    const xmlpp::Element* formula_node,
    const std::string& base_path) {
  std::string type = formula_node->get_name();
  if (type == "event" || type == "basic-event" || type == "gate" ||
      type == "house-event") {
    type = "null";
  }
  FormulaPtr formula(new Formula(type));
  if (type == "atleast") {
    std::string min_num = formula_node->get_attribute_value("min");
    boost::trim(min_num);
    int vote_number = boost::lexical_cast<int>(min_num);
    formula->vote_number(vote_number);
  }
  // Process arguments of this formula.
  if (type == "null") {  // Special case of pass-through.
    formula_node = formula_node->get_parent();
  }
  Initializer::ProcessFormula(formula_node, base_path, formula.get());

  try {
    formula->Validate();
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << formula_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
  return formula;
}

void Initializer::ProcessFormula(const xmlpp::Element* formula_node,
                                 const std::string& base_path,
                                 Formula* formula) {
  xmlpp::NodeSet events = formula_node->find("./*[name() = 'event' or "
                                             "name() = 'gate' or "
                                             "name() = 'basic-event' or "
                                             "name() = 'house-event']");
  xmlpp::NodeSet::const_iterator it;
  for (it = events.begin(); it != events.end(); ++it) {
    const xmlpp::Element* event = static_cast<const xmlpp::Element*>(*it);
    std::string name = event->get_attribute_value("name");
    boost::trim(name);

    std::string element_type = event->get_name();
    // This is for the case "<event name="id" type="type"/>".
    std::string type = event->get_attribute_value("type");
    boost::trim(type);
    if (type != "") {
      assert(type == "gate" || type == "basic-event" || type == "house-event");
      element_type = type;  // Event type is defined.
    }

    try {
      if (element_type == "event") {  // Undefined type yet.
        std::pair<EventPtr, std::string> target =
            model_->GetEvent(name, base_path);
        EventPtr event = target.first;
        event->orphan(false);
        std::string type = target.second;
        if (type == "gate") {
          formula->AddArgument(std::static_pointer_cast<Gate>(event));
        } else if (type == "basic-event") {
          formula->AddArgument(std::static_pointer_cast<BasicEvent>(event));
        } else {
          assert(type == "house-event");
          formula->AddArgument(std::static_pointer_cast<HouseEvent>(event));
        }
      } else if (element_type == "gate") {
        GatePtr gate = model_->GetGate(name, base_path);
        formula->AddArgument(gate);
        gate->orphan(false);

      } else if (element_type == "basic-event") {
        BasicEventPtr basic_event = model_->GetBasicEvent(name, base_path);
        formula->AddArgument(basic_event);
        basic_event->orphan(false);

      } else {
        assert(element_type == "house-event");
        HouseEventPtr house_event = model_->GetHouseEvent(name, base_path);
        formula->AddArgument(house_event);
        house_event->orphan(false);
      }
    } catch (ValidationError& err) {
      std::stringstream msg;
      msg << "Line " << event->get_line() << ":\n";
      err.msg(msg.str() + err.msg());
      throw err;
    }
  }

  xmlpp::NodeSet formulas = formula_node->find("./*[name() != 'event' and "
                                               "name() != 'gate' and "
                                               "name() != 'basic-event' and "
                                               "name() != 'house-event']");
  for (it = formulas.begin(); it != formulas.end(); ++it) {
    const xmlpp::Element* nested_formula =
        static_cast<const xmlpp::Element*>(*it);
    formula->AddArgument(Initializer::GetFormula(nested_formula, base_path));
  }
}

std::shared_ptr<BasicEvent> Initializer::RegisterBasicEvent(
    const xmlpp::Element* event_node,
    const std::string& base_path,
    bool public_container) {
  std::string name = event_node->get_attribute_value("name");
  boost::trim(name);
  std::string role = event_node->get_attribute_value("role");
  boost::trim(role);
  bool event_role = public_container;  // Inherited role by default.
  if (role != "") event_role = role == "public" ? true : false;
  BasicEventPtr basic_event(new BasicEvent(name, base_path, event_role));
  try {
    model_->AddBasicEvent(basic_event);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
  tbd_.basic_events.push_back(std::make_pair(basic_event, event_node));
  Initializer::AttachLabelAndAttributes(event_node, basic_event.get());
  return basic_event;
}

void Initializer::DefineBasicEvent(const xmlpp::Element* event_node,
                                   const BasicEventPtr& basic_event) {
  xmlpp::NodeSet expressions =
     event_node->find("./*[name() != 'attributes' and name() != 'label']");

  if (!expressions.empty()) {
    const xmlpp::Element* expr_node =
        static_cast<const xmlpp::Element*>(expressions.back());
    ExpressionPtr expression =
        Initializer::GetExpression(expr_node, basic_event->base_path());
    basic_event->expression(expression);
  }
}

std::shared_ptr<HouseEvent> Initializer::DefineHouseEvent(
    const xmlpp::Element* event_node,
    const std::string& base_path,
    bool public_container) {
  std::string name = event_node->get_attribute_value("name");
  boost::trim(name);
  std::string role = event_node->get_attribute_value("role");
  boost::trim(role);
  bool event_role = public_container;  // Inherited role by default.
  if (role != "") event_role = role == "public" ? true : false;
  HouseEventPtr house_event(new HouseEvent(name, base_path, event_role));
  try {
    model_->AddHouseEvent(house_event);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }

  // Only Boolean constant.
  xmlpp::NodeSet expression = event_node->find("./constant");
  if (!expression.empty()) {
    assert(expression.size() == 1);
    const xmlpp::Element* constant =
        static_cast<const xmlpp::Element*>(expression.front());

    std::string val = constant->get_attribute_value("value");
    boost::trim(val);
    assert(val == "true" || val == "false");
    bool state = (val == "true") ? true : false;
    house_event->state(state);
  }
  Initializer::AttachLabelAndAttributes(event_node, house_event.get());
  return house_event;
}

std::shared_ptr<Parameter> Initializer::RegisterParameter(
    const xmlpp::Element* param_node,
    const std::string& base_path,
    bool public_container) {
  std::string name = param_node->get_attribute_value("name");
  boost::trim(name);
  std::string role = param_node->get_attribute_value("role");
  boost::trim(role);
  bool param_role = public_container;  // Inherited role by default.
  if (role != "") param_role = role == "public" ? true : false;
  ParameterPtr parameter(new Parameter(name, base_path, param_role));
  try {
    model_->AddParameter(parameter);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << param_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
  tbd_.parameters.push_back(std::make_pair(parameter, param_node));

  // Attach units.
  std::string unit = param_node->get_attribute_value("unit");
  boost::trim(unit);
  if (unit != "") {
    assert(kUnits_.count(unit));
    parameter->unit(kUnits_.find(unit)->second);
  }
  Initializer::AttachLabelAndAttributes(param_node, parameter.get());
  return parameter;
}

void Initializer::DefineParameter(const xmlpp::Element* param_node,
                                  const ParameterPtr& parameter) {
  // Assuming that expression is the last child of the parameter definition.
  xmlpp::NodeSet expressions =
      param_node->find("./*[name() != 'attributes' and name() != 'label']");
  assert(expressions.size() == 1);
  const xmlpp::Element* expr_node =
      static_cast<const xmlpp::Element*>(expressions.back());
  ExpressionPtr expression =
      Initializer::GetExpression(expr_node, parameter->base_path());

  parameter->expression(expression);
}

std::shared_ptr<Expression> Initializer::GetExpression(
    const xmlpp::Element* expr_element,
    const std::string& base_path) {
  ExpressionPtr expression;
  bool not_parameter = true;  // Parameters are saved in a different container.
  if (GetConstantExpression(expr_element, expression)) {
  } else if (GetParameterExpression(expr_element, base_path, expression)) {
    not_parameter = false;
  } else {
    GetDeviateExpression(expr_element, base_path, expression);
  }
  assert(expression);
  if (not_parameter) expressions_.push_back(expression);
  return expression;
}

bool Initializer::GetConstantExpression(const xmlpp::Element* expr_element,
                                        ExpressionPtr& expression) {
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  if (expr_name == "float" || expr_name == "int") {
    std::string val = expr_element->get_attribute_value("value");
    boost::trim(val);
    double num = boost::lexical_cast<double>(val);
    expression = std::make_shared<ConstantExpression>(num);

  } else if (expr_name == "bool") {
    std::string val = expr_element->get_attribute_value("value");
    boost::trim(val);
    bool state = (val == "true") ? true : false;
    expression = std::make_shared<ConstantExpression>(state);
  } else {
    return false;
  }
  return true;
}

bool Initializer::GetParameterExpression(const xmlpp::Element* expr_element,
                                         const std::string& base_path,
                                         ExpressionPtr& expression) {
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  std::string param_unit = "";  // The expected unit.
  if (expr_name == "parameter") {
    std::string name = expr_element->get_attribute_value("name");
    boost::trim(name);
    try {
      ParameterPtr param = model_->GetParameter(name, base_path);
      param->unused(false);
      param_unit = kUnitToString_[param->unit()];
      expression = param;
    } catch (ValidationError& err) {
      std::stringstream msg;
      msg << "Line " << expr_element->get_line() << ":\n";
      err.msg(msg.str() + err.msg());
      throw err;
    }
  } else if (expr_name == "system-mission-time") {
    param_unit = kUnitToString_[mission_time_->unit()];
    expression = mission_time_;

  } else {
    return false;
  }
  // Check units.
  std::string unit = expr_element->get_attribute_value("unit");
  boost::trim(unit);
  if (!unit.empty() && unit != param_unit) {
    std::stringstream msg;
    msg << "Line " << expr_element->get_line() << ":\n";
    msg << "Parameter unit mismatch.\nExpected: " << param_unit
        << "\nGiven: " << unit;
    throw ValidationError(msg.str());
  }
  return true;
}

bool Initializer::GetDeviateExpression(const xmlpp::Element* expr_element,
                                       const std::string& base_path,
                                       ExpressionPtr& expression) {
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  xmlpp::NodeSet args = expr_element->find("./*");
  if (expr_name == "uniform-deviate") {
    assert(args.size() == 2);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr min = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr max = GetExpression(element, base_path);

    expression = std::shared_ptr<UniformDeviate>(new UniformDeviate(min, max));

  } else if (expr_name == "normal-deviate") {
    assert(args.size() == 2);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr mean = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr sigma = GetExpression(element, base_path);

    expression = std::shared_ptr<NormalDeviate>(new NormalDeviate(mean, sigma));

  } else if (expr_name == "lognormal-deviate") {
    assert(args.size() == 3);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr mean = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr ef = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[2]);
    ExpressionPtr level = GetExpression(element, base_path);

    expression = std::shared_ptr<LogNormalDeviate>(
        new LogNormalDeviate(mean, ef, level));

  } else if (expr_name == "gamma-deviate") {
    assert(args.size() == 2);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr k = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr theta = GetExpression(element, base_path);

    expression = std::shared_ptr<GammaDeviate>(new GammaDeviate(k, theta));

  } else if (expr_name == "beta-deviate") {
    assert(args.size() == 2);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr alpha = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr beta = GetExpression(element, base_path);

    expression = std::shared_ptr<BetaDeviate>(new BetaDeviate(alpha, beta));

  } else if (expr_name == "histogram") {
    std::vector<ExpressionPtr> boundaries;
    std::vector<ExpressionPtr> weights;
    xmlpp::NodeSet::iterator it;
    for (it = args.begin(); it != args.end(); ++it) {
      const xmlpp::Element* el = static_cast<const xmlpp::Element*>(*it);
      xmlpp::NodeSet pair = el->find("./*");
      assert(pair.size() == 2);
      const xmlpp::Element* element =
          static_cast<const xmlpp::Element*>(pair[0]);
      ExpressionPtr bound = GetExpression(element, base_path);
      boundaries.push_back(bound);

      element = static_cast<const xmlpp::Element*>(pair[1]);
      ExpressionPtr weight = GetExpression(element, base_path);
      weights.push_back(weight);
    }
    expression = std::shared_ptr<Histogram>(new Histogram(boundaries, weights));

  } else if (expr_name == "exponential") {
    assert(args.size() == 2);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr lambda = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr time = GetExpression(element, base_path);

    expression = std::shared_ptr<ExponentialExpression>(
        new ExponentialExpression(lambda, time));

  } else if (expr_name == "GLM") {
    assert(args.size() == 4);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr gamma = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr lambda = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[2]);
    ExpressionPtr mu = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[3]);
    ExpressionPtr time = GetExpression(element, base_path);

    expression = std::shared_ptr<GlmExpression>(
        new GlmExpression(gamma, lambda, mu, time));

  } else if (expr_name == "Weibull") {
    assert(args.size() == 4);
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(args[0]);
    ExpressionPtr alpha = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[1]);
    ExpressionPtr beta = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[2]);
    ExpressionPtr t0 = GetExpression(element, base_path);

    element = static_cast<const xmlpp::Element*>(args[3]);
    ExpressionPtr time = GetExpression(element, base_path);

    expression = std::shared_ptr<WeibullExpression>(
        new WeibullExpression(alpha, beta, t0, time));
  } else {
    return false;
  }
  return true;
}

std::shared_ptr<CcfGroup> Initializer::RegisterCcfGroup(
    const xmlpp::Element* ccf_node,
    const std::string& base_path,
    bool public_container) {
  std::string name = ccf_node->get_attribute_value("name");
  boost::trim(name);
  std::string model = ccf_node->get_attribute_value("model");
  boost::trim(model);
  assert(model == "beta-factor" || model == "alpha-factor" ||
         model == "MGL" || model == "phi-factor");

  CcfGroupPtr ccf_group;
  if (model == "beta-factor") {
    ccf_group = CcfGroupPtr(new BetaFactorModel(name, base_path,
                                                public_container));

  } else if (model == "MGL") {
    ccf_group = CcfGroupPtr(new MglModel(name, base_path, public_container));

  } else if (model == "alpha-factor") {
    ccf_group = CcfGroupPtr(new AlphaFactorModel(name, base_path,
                                                 public_container));

  } else if (model == "phi-factor") {
    ccf_group = CcfGroupPtr(new PhiFactorModel(name, base_path,
                                               public_container));
  }

  try {
    model_->AddCcfGroup(ccf_group);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << ccf_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }

  xmlpp::NodeSet members = ccf_node->find("./members");
  assert(members.size() == 1);
  const xmlpp::Element* element =
      static_cast<const xmlpp::Element*>(members[0]);

  Initializer::ProcessCcfMembers(element, ccf_group);

  Initializer::AttachLabelAndAttributes(ccf_node, ccf_group.get());

  tbd_.ccf_groups.push_back(std::make_pair(ccf_group, ccf_node));
  return ccf_group;
}

void Initializer::DefineCcfGroup(const xmlpp::Element* ccf_node,
                                 const CcfGroupPtr& ccf_group) {
  xmlpp::NodeSet children = ccf_node->find("./*");
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* element = static_cast<const xmlpp::Element*>(*it);
    std::string name = element->get_name();

    if (name == "distribution") {
      assert(element->find("./*").size() == 1);
      const xmlpp::Element* expr_node =
          static_cast<const xmlpp::Element*>(element->find("./*")[0]);
      ExpressionPtr expression =
          Initializer::GetExpression(expr_node, ccf_group->base_path());
      ccf_group->AddDistribution(expression);

    } else if (name == "factor") {
      Initializer::DefineCcfFactor(element, ccf_group);

    } else if (name == "factors") {
      Initializer::ProcessCcfFactors(element, ccf_group);
    }
  }
}

void Initializer::ProcessCcfMembers(const xmlpp::Element* members_node,
                                    const CcfGroupPtr& ccf_group) {
  xmlpp::NodeSet children = members_node->find("./*");
  assert(!children.empty());
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* event_node = static_cast<const xmlpp::Element*>(*it);
    assert("basic-event" == event_node->get_name());

    std::string name = event_node->get_attribute_value("name");
    boost::trim(name);
    BasicEventPtr basic_event(new BasicEvent(name, ccf_group->base_path(),
                                             ccf_group->is_public()));
    try {
      ccf_group->AddMember(basic_event);
      model_->AddBasicEvent(basic_event);
    } catch (DuplicateArgumentError& err) {
      std::stringstream msg;
      msg << "Line " << event_node->get_line() << ":\n";
      err.msg(msg.str() + err.msg());
      throw err;
    }
  }
}

void Initializer::ProcessCcfFactors(const xmlpp::Element* factors_node,
                                    const CcfGroupPtr& ccf_group) {
  xmlpp::NodeSet children = factors_node->find("./*");
  assert(!children.empty());
  // To keep track of CCF group factor levels.
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* factor_node = static_cast<const xmlpp::Element*>(*it);
    Initializer::DefineCcfFactor(factor_node, ccf_group);
  }
}

void Initializer::DefineCcfFactor(const xmlpp::Element* factor_node,
                                  const CcfGroupPtr& ccf_group) {
  // Checking the level for one factor input.
  std::string level = factor_node->get_attribute_value("level");
  boost::trim(level);
  if (level.empty()) {
    std::stringstream msg;
    msg << "Line " << factor_node->get_line() << ":\n";
    msg << "CCF group factor level number is not provided.";
    throw ValidationError(msg.str());
  }
  int level_num = boost::lexical_cast<int>(level);
  assert(factor_node->find("./*").size() == 1);
  const xmlpp::Element* expr_node =
      static_cast<const xmlpp::Element*>(factor_node->find("./*")[0]);
  ExpressionPtr expression =
      Initializer::GetExpression(expr_node, ccf_group->base_path());
  try {
    ccf_group->AddFactor(expression, level_num);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << factor_node->get_line() << ":\n";
    err.msg(msg.str() + err.msg());
    throw err;
  }
}

void Initializer::ValidateInitialization() {
  // Validation of essential members of analysis in the first layer.
  Initializer::CheckFirstLayer();

  // Validation of fault trees.
  Initializer::CheckSecondLayer();
}

void Initializer::CheckFirstLayer() {
  // Check if all gates have no cycles.
  std::unordered_map<std::string, GatePtr>::const_iterator it;
  for (it = model_->gates().begin(); it != model_->gates().end(); ++it) {
    std::vector<std::string> cycle;
    if (cycle::DetectCycle<Gate, Formula>(it->second.get(), &cycle)) {
      std::string msg = "Detected a cycle in " + it->second->name() +
                        " gate:\n";
      msg += cycle::PrintCycle(cycle);
      throw ValidationError(msg);
    }
  }
  std::stringstream error_messages;
  // Check if all primary events have expressions for probability analysis.
  if (settings_.probability_analysis()) {
    std::string msg = "";
    std::unordered_map<std::string, BasicEventPtr>::const_iterator it_b;
    for (it_b = model_->basic_events().begin();
         it_b != model_->basic_events().end(); ++it_b) {
      if (!it_b->second->has_expression()) msg += it_b->second->name() + "\n";
    }
    std::unordered_map<std::string, HouseEventPtr>::const_iterator it_h;
    for (it_h = model_->house_events().begin();
         it_h != model_->house_events().end(); ++it_h) {
      if (!it_h->second->has_expression()) msg += it_h->second->name() + "\n";
    }
    if (!msg.empty()) {
      error_messages << "\nThese primary events do not have expressions:\n"
                     << msg;
    }
  }

  if (!error_messages.str().empty()) {
    throw ValidationError(error_messages.str());
  }

  Initializer::ValidateExpressions();
}

void Initializer::CheckSecondLayer() {
  if (!model_->ccf_groups().empty()) {
    std::unordered_map<std::string, CcfGroupPtr>::const_iterator it;
    for (it = model_->ccf_groups().begin(); it != model_->ccf_groups().end();
         ++it) {
      it->second->Validate();
    }
  }
}

void Initializer::ValidateExpressions() {
  // Check for cycles in parameters. This must be done before expressions.
  if (!model_->parameters().empty()) {
    std::unordered_map<std::string, ParameterPtr>::const_iterator it;
    for (it = model_->parameters().begin(); it != model_->parameters().end();
         ++it) {
      std::vector<std::string> cycle;
      if (cycle::DetectCycle<Parameter, Expression>(it->second.get(), &cycle)) {
        std::string msg = "Detected a cycle in " + it->second->name() +
                          " parameter:\n";
        msg += cycle::PrintCycle(cycle);
        throw ValidationError(msg);
      }
    }
  }

  // Validate expressions.
  if (!expressions_.empty()) {
    try {
      std::vector<ExpressionPtr>::iterator it;
      for (it = expressions_.begin(); it != expressions_.end(); ++it) {
        (*it)->Validate();
      }
    } catch (InvalidArgument& err) {
      throw ValidationError(err.msg());
    }
  }

  // Check probability values for primary events.
  if (settings_.probability_analysis()) {
    std::stringstream msg;
    msg << "";
    if (!model_->ccf_groups().empty()) {
      std::unordered_map<std::string, CcfGroupPtr>::const_iterator it;
      for (it = model_->ccf_groups().begin(); it != model_->ccf_groups().end();
           ++it) {
        try {
          it->second->ValidateDistribution();
        } catch (ValidationError& err) {
          msg << it->second->name() << " : " << err.msg() << "\n";
        }
      }
    }
    std::unordered_map<std::string, BasicEventPtr>::const_iterator it;
    for (it = model_->basic_events().begin();
         it != model_->basic_events().end(); ++it) {
      try {
        it->second->Validate();
      } catch (ValidationError& err) {
        msg << it->second->name() << " : " << err.msg() << "\n";
      }
    }
    if (!msg.str().empty()) {
      std::string head = "Invalid probabilities detected:\n";
      throw ValidationError(head + msg.str());
    }
  }
}

void Initializer::SetupForAnalysis() {
  // Collecting top events of fault trees.
  if (!model_->fault_trees().empty()) {
    std::unordered_map<std::string, FaultTreePtr>::const_iterator it;
    for (it = model_->fault_trees().begin(); it != model_->fault_trees().end();
         ++it) {
      it->second->CollectTopEvents();
    }
  }

  // CCF groups must apply models to basic event members.
  if (!model_->ccf_groups().empty()) {
    std::unordered_map<std::string, CcfGroupPtr>::const_iterator it;
    for (it = model_->ccf_groups().begin(); it != model_->ccf_groups().end();
         ++it) {
      it->second->ApplyModel();
    }
  }
}

}  // namespace scram
