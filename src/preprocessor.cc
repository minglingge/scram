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
/// @file preprocessor.cc
/// Implementation of preprocessing algorithms. If a preprocessing algorithm has
/// its limitations, side-effects, and assumptions, the documentation in the
/// header file must contain all the relevant information within notes or
/// warnings. The default assumption for all algorithms is that the fault
/// tree is valid and well-formed.
///
/// Some Suggested Notes/Warnings: (Clear contract for preprocessing algorithms)
///
///   * Coherent trees only
///   * Positive gates or nodes only
///   * Node visits or gate marks must be cleared before the call
///   * May introduce NULL or UNITY state gates or constants
///   * May introduce NULL/NOT type gates
///   * Operates on certain gate types only
///   * Normalized gates only
///   * Should not have gates of certain types
///   * How it deals with modules (Aware of them or not at all)
///   * Should not have constants or constant gates
///   * Does it depend on other preprocessing functions?
///   * Does it swap the root gate of the graph with another (arg) gate?
///   * Does it remove gates or other kind of nodes?
///
/// Assuming that the fault tree is provided in the state as described in the
/// contract, the algorithms should never throw an exception. The algorithms
/// must guarantee that, given a valid and well-formed fault tree, the resulting
/// fault tree will at least be valid, well-formed, and semantically equivalent
/// to the input fault tree.
///
/// If the contract is not respected, the result or behavior of the algorithm
/// may be undefined. There is no requirement to check for the broken contract
/// and to exit gracefully.
#include "preprocessor.h"

#include <algorithm>
#include <queue>

#include "logger.h"

namespace scram {

Preprocessor::Preprocessor(BooleanGraph* graph)
    : graph_(graph),
      root_sign_(1),
      constants_(graph->constants()) {}

void Preprocessor::ProcessFaultTree() {
  IGatePtr root = graph_->root();
  assert(root);
  assert(root->parents().empty());
  assert(!root->mark());

  CLOCK(prep_time);  // Overall preprocessing time.
  LOG(DEBUG2) << "Preprocessing...";

  if (constants_) {
    LOG(DEBUG2) << "Propagating constants...";
    Preprocessor::PropagateConstants(root);
    LOG(DEBUG2) << "Constant propagation is done!";
  }

  if (!graph_->normal()) {
    LOG(DEBUG2) << "Normalizing gates...";
    assert(root_sign_ == 1);
    Preprocessor::NormalizeGates();
    LOG(DEBUG2) << "Finished normalizing gates!";
  }

  Preprocessor::RemoveNullGates();  /// @todo Run before normalization.

  if (root->state() != kNormalState) {  // The root gate has become constant.
    if (root_sign_ < 0) {
      State orig_state = root->state();
      root = IGatePtr(new IGate(kNullGate));
      graph_->root(root);
      if (orig_state == kNullState) {
        root->MakeUnity();
      } else {
        assert(orig_state == kUnityState);
        root->Nullify();
      }
      root_sign_ = 1;
    }
    return;
  }
  if (root->type() == kNullGate) {  // Special case of preprocessing.
    assert(root->args().size() == 1);
    if (!root->gate_args().empty()) {
      int signed_index = root->gate_args().begin()->first;
      IGatePtr arg = root->gate_args().begin()->second;
      graph_->root(arg);
      root = arg;
      assert(root->parents().empty());
      assert(root->type() == kOrGate || root->type() == kAndGate);
      root_sign_ *= signed_index > 0 ? 1 : -1;
    }
  }
  if (!graph_->coherent()) {
    LOG(DEBUG2) << "Propagating complements...";
    if (root_sign_ < 0) {
      assert(root->type() == kOrGate || root->type() == kAndGate ||
             root->type() == kNullGate);
      if (root->type() == kOrGate || root->type() == kAndGate)
        root->type(root->type() == kOrGate ? kAndGate : kOrGate);
      root->InvertArgs();
      root_sign_ = 1;
    }
    std::map<int, IGatePtr> complements;
    Preprocessor::ClearGateMarks();
    Preprocessor::PropagateComplements(root, &complements);
    LOG(DEBUG2) << "Complement propagation is done!";
  }

  CLOCK(mult_time);
  LOG(DEBUG2) << "Detecting multiple definitions...";
  bool tree_changed = true;
  while (tree_changed) {
    tree_changed = Preprocessor::ProcessMultipleDefinitions();
  }
  LOG(DEBUG2) << "Finished multi-definition detection in " << DUR(mult_time);

  if (graph_->coherent()) {
    Preprocessor::ClearGateMarks();
    Preprocessor::BooleanOptimization();
  }

  LOG(DEBUG2) << "Coalescing gates...";
  Preprocessor::ClearGateMarks();
  Preprocessor::RemoveNullGates();
  tree_changed = true;
  while (tree_changed) {
    assert(const_gates_.empty());
    assert(null_gates_.empty());

    tree_changed = false;
    Preprocessor::ClearGateMarks();
    Preprocessor::JoinGates(root);  // Registers const gates.

    if (!const_gates_.empty()) {
      Preprocessor::ClearConstGates();
      tree_changed = true;
    }
  }
  LOG(DEBUG2) << "Gate coalescense is done!";

  // After this point there should not be null AND or unity OR gates,
  // and the tree structure should be repeating OR and AND.
  // All gates are positive, and each gate has at least two arguments.
  if (root->args().empty()) return;  // This is null or unity.
  // Detect original modules for processing.
  Preprocessor::DetectModules();
  LOG(DEBUG2) << "Finished preprocessing in " << DUR(prep_time);
}

void Preprocessor::NormalizeGates() {
  assert(const_gates_.empty());
  assert(null_gates_.empty());
  // Handle special case for the root gate.
  IGatePtr root_gate = graph_->root();
  Operator type = root_gate->type();
  switch (type) {
    case kNorGate:
    case kNandGate:
    case kNotGate:
      root_sign_ *= -1;
  }
  // Process negative gates. Note that root's negative gate is processed in
  // the above lines. All arguments are assumed to be positive at this point.
  Preprocessor::ClearGateMarks();
  Preprocessor::NotifyParentsOfNegativeGates(root_gate);

  Preprocessor::ClearGateMarks();
  Preprocessor::NormalizeGate(root_gate);  // Registers null gates only.

  assert(const_gates_.empty());
  if (!null_gates_.empty()) Preprocessor::ClearNullGates();
}

void Preprocessor::NotifyParentsOfNegativeGates(const IGatePtr& gate) {
  if (gate->mark()) return;
  gate->mark(true);
  std::vector<int> to_negate;  // Args to get the negation.
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    IGatePtr arg = it->second;
    Preprocessor::NotifyParentsOfNegativeGates(arg);
    switch (arg->type()) {
      case kNorGate:
      case kNandGate:
      case kNotGate:
        to_negate.push_back(it->first);
    }
  }
  std::vector<int>::iterator it_neg;
  for (it_neg = to_negate.begin(); it_neg != to_negate.end(); ++it_neg) {
    gate->InvertArg(*it_neg);  // Does not produce constants or duplicates.
  }
}

void Preprocessor::NormalizeGate(const IGatePtr& gate) {
  if (gate->mark()) return;
  gate->mark(true);
  assert(gate->state() == kNormalState);
  assert(!gate->args().empty());
  // Depth-first traversal before the arguments may get changed.
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    Preprocessor::NormalizeGate(it->second);
  }

  switch (gate->type()) {  // Negation is already processed.
    case kNotGate:
      assert(gate->args().size() == 1);
      gate->type(kNullGate);
      break;
    case kNorGate:
    case kOrGate:
      assert(gate->args().size() > 1);
      gate->type(kOrGate);
      break;
    case kNandGate:
    case kAndGate:
      assert(gate->args().size() > 1);
      gate->type(kAndGate);
      break;
    case kXorGate:
      assert(gate->args().size() == 2);
      Preprocessor::NormalizeXorGate(gate);
      break;
    case kAtleastGate:
      assert(gate->args().size() > 2);
      assert(gate->vote_number() > 1);
      Preprocessor::NormalizeAtleastGate(gate);
      break;
    case kNullGate:
      null_gates_.push_back(gate);  // Register for removal.
      break;
  }
}

void Preprocessor::NormalizeXorGate(const IGatePtr& gate) {
  assert(gate->args().size() == 2);
  IGatePtr gate_one(new IGate(kAndGate));
  IGatePtr gate_two(new IGate(kAndGate));
  gate_one->mark(true);
  gate_two->mark(true);

  gate->type(kOrGate);
  std::set<int>::const_iterator it = gate->args().begin();
  gate->ShareArg(*it, gate_one);
  gate->ShareArg(*it, gate_two);
  gate_two->InvertArg(*it);

  ++it;  // Handling the second argument.
  gate->ShareArg(*it, gate_one);
  gate_one->InvertArg(*it);
  gate->ShareArg(*it, gate_two);

  gate->EraseAllArgs();
  gate->AddArg(gate_one->index(), gate_one);
  gate->AddArg(gate_two->index(), gate_two);
}

void Preprocessor::NormalizeAtleastGate(const IGatePtr& gate) {
  assert(gate->type() == kAtleastGate);
  int vote_number = gate->vote_number();

  assert(vote_number > 0);  // Vote number can be 1 for special OR gates.
  assert(gate->args().size() > 1);
  if (gate->args().size() == vote_number) {
    gate->type(kAndGate);
    return;
  } else if (vote_number == 1) {
    gate->type(kOrGate);
    return;
  }

  const std::set<int>& args = gate->args();
  std::set<int>::const_iterator it = args.begin();

  IGatePtr first_arg(new IGate(kAndGate));
  gate->ShareArg(*it, first_arg);

  IGatePtr grand_arg(new IGate(kAtleastGate));
  first_arg->AddArg(grand_arg->index(), grand_arg);
  grand_arg->vote_number(vote_number - 1);

  IGatePtr second_arg(new IGate(kAtleastGate));
  second_arg->vote_number(vote_number);

  for (++it; it != args.end(); ++it) {
    gate->ShareArg(*it, grand_arg);
    gate->ShareArg(*it, second_arg);
  }

  first_arg->mark(true);
  second_arg->mark(true);
  grand_arg->mark(true);

  gate->type(kOrGate);
  gate->EraseAllArgs();
  gate->AddArg(first_arg->index(), first_arg);
  gate->AddArg(second_arg->index(), second_arg);

  Preprocessor::NormalizeAtleastGate(grand_arg);
  Preprocessor::NormalizeAtleastGate(second_arg);
}

void Preprocessor::PropagateConstGate(const IGatePtr& gate) {
  assert(gate->state() != kNormalState);

  while(!gate->parents().empty()) {
    IGatePtr parent = gate->parents().begin()->second.lock();

    int sign = parent->args().count(gate->index()) ? 1 : -1;
    bool state = gate->state() == kNullState ? false : true;
    if (sign < 0) state = !state;

    std::vector<int> to_erase;
    Preprocessor::ProcessConstantArg(parent, sign * gate->index(),
                                     state, &to_erase);
    Preprocessor::RemoveArgs(parent, to_erase);

    if (parent->state() != kNormalState) {
      Preprocessor::PropagateConstGate(parent);
    } else if (parent->type() == kNullGate) {
      Preprocessor::PropagateNullGate(parent);
    }
  }
}

void Preprocessor::PropagateNullGate(const IGatePtr& gate) {
  assert(gate->type() == kNullGate);

  while(!gate->parents().empty()) {
    IGatePtr parent = gate->parents().begin()->second.lock();
    int sign = parent->args().count(gate->index()) ? 1 : -1;
    parent->JoinNullGate(sign * gate->index());

    if (parent->state() != kNormalState) {
      Preprocessor::PropagateConstGate(parent);
    } else if (parent->type() == kNullGate) {
      Preprocessor::PropagateNullGate(parent);
    }
  }
}

void Preprocessor::ClearConstGates() {
  Preprocessor::ClearGateMarks();  // New gates may get created without marks!
  std::vector<boost::weak_ptr<IGate> >::iterator it;
  for (it = const_gates_.begin(); it != const_gates_.end(); ++it) {
    if (it->expired()) continue;
    Preprocessor::PropagateConstGate(it->lock());
  }
  const_gates_.clear();
}

void Preprocessor::ClearNullGates() {
  Preprocessor::ClearGateMarks();  // New gates may get created without marks!
  std::vector<boost::weak_ptr<IGate> >::iterator it;
  for (it = null_gates_.begin(); it != null_gates_.end(); ++it) {
    if (it->expired()) continue;
    Preprocessor::PropagateNullGate(it->lock());
  }
  null_gates_.clear();
}

bool Preprocessor::PropagateConstants(const IGatePtr& gate) {
  if (gate->mark()) return false;
  gate->mark(true);
  if (gate->state() != kNormalState) return false;

  bool changed = false;  // Indication if this operation changed the gate.
  std::vector<int> to_erase;  // Erase arguments later to keep iterators valid.
  boost::unordered_map<int, ConstantPtr>::const_iterator it_c;
  for (it_c = gate->constant_args().begin();
       it_c != gate->constant_args().end(); ++it_c) {
    int index = it_c->first;  // May be negation.
    bool state = it_c->second->state();
    if (index < 0) state = !state;
    if (Preprocessor::ProcessConstantArg(gate, index, state, &to_erase))
      return true;  // The parent gate itself has become constant.
  }
  boost::unordered_map<int, IGatePtr>::const_iterator it_g;
  for (it_g = gate->gate_args().begin(); it_g != gate->gate_args().end();
       ++it_g) {
    IGatePtr arg_gate = it_g->second;
    bool ret = Preprocessor::PropagateConstants(arg_gate);
    if (!changed && ret) changed = true;

    State gate_state = arg_gate->state();
    if (gate_state == kNormalState) continue;
    int index = it_g->first;  // May be negation.
    bool state = gate_state == kNullState ? false : true;
    if (index < 0) state = !state;
    if (Preprocessor::ProcessConstantArg(gate, index, state, &to_erase))
      return true;  // Early exit because the parent has become constant.
  }
  if (!changed && !to_erase.empty()) changed = true;
  Preprocessor::RemoveArgs(gate, to_erase);
  return changed;
}

bool Preprocessor::ProcessConstantArg(const IGatePtr& gate, int arg,
                                      bool state, std::vector<int>* to_erase) {
  Operator parent_type = gate->type();

  if (!state) {  // Null state arg.
    switch (parent_type) {
      case kNorGate:
      case kXorGate:
      case kOrGate:
        to_erase->push_back(arg);
        return false;
      case kNullGate:
      case kAndGate:
        gate->Nullify();
        break;
      case kNandGate:
      case kNotGate:
        gate->MakeUnity();
        break;
      case kAtleastGate:  // K / (N - 1).
        to_erase->push_back(arg);
        int k = gate->vote_number();
        int n = gate->args().size() - to_erase->size();
        if (k == n) gate->type(kAndGate);
        return false;
    }
  } else {  // Unity state arg.
    switch (parent_type) {
      case kNullGate:
      case kOrGate:
        gate->MakeUnity();
        break;
      case kNandGate:
      case kAndGate:
        to_erase->push_back(arg);
        return false;
      case kNorGate:
      case kNotGate:
        gate->Nullify();
        break;
      case kXorGate:  // Special handling due to its internal negation.
        assert(gate->args().size() == 2);
        if (to_erase->size() == 1) {  // The other arg is NULL.
          gate->MakeUnity();
        } else {
          assert(to_erase->empty());
          gate->type(kNotGate);
          to_erase->push_back(arg);
          return false;
        }
        break;
      case kAtleastGate:  // (K - 1) / (N - 1).
        int k = gate->vote_number();
        --k;
        if (k == 1) gate->type(kOrGate);
        assert(k > 1);
        gate->vote_number(k);
        to_erase->push_back(arg);
        return false;
    }
  }
  return true;  // Becomes constant NULL or UNITY most of the cases.
}

void Preprocessor::RemoveArgs(const IGatePtr& gate,
                              const std::vector<int>& to_erase) {
  if (to_erase.empty()) return;
  assert(to_erase.size() <= gate->args().size());
  std::vector<int>::const_iterator it_v;
  for (it_v = to_erase.begin(); it_v != to_erase.end(); ++it_v) {
    gate->EraseArg(*it_v);
  }
  Operator type = gate->type();
  if (gate->args().empty()) {
    assert(type != kNotGate && type != kNullGate);  // Constant by design.
    assert(type != kAtleastGate);  // Must get transformed by design.
    switch (type) {
      case kNandGate:
      case kXorGate:
      case kOrGate:
        gate->Nullify();
        break;
      case kNorGate:
      case kAndGate:
        gate->MakeUnity();
        break;
    }
  } else if (gate->args().size() == 1) {
    assert(type != kAtleastGate);  // Cannot have only one arg by processing.
    switch (type) {
      case kXorGate:
      case kOrGate:
      case kAndGate:
        gate->type(kNullGate);
        break;
      case kNorGate:
      case kNandGate:
        gate->type(kNotGate);
        break;
      default:
        assert(type == kNotGate || type == kNullGate);
    }
  }
}

void Preprocessor::PropagateComplements(
    const IGatePtr& gate,
    std::map<int, IGatePtr>* gate_complements) {
  if (gate->mark()) return;
  gate->mark(true);
  //assert(gate->args().size() > 1);  /// @todo Put Back.
  // If the argument gate is complement, then create a new gate that propagates
  // its sign to its arguments and itself becomes non-complement.
  // Keep track of complement gates for optimization of repeated complements.
  std::vector<int> to_swap;  // Args with negation to get swapped.
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    IGatePtr arg_gate = it->second;
    if (it->first < 0) {
      to_swap.push_back(it->first);
      if (gate_complements->count(arg_gate->index())) continue;
      Operator type = arg_gate->type();
      assert(type == kAndGate || type == kOrGate);
      Operator complement_type = type == kOrGate ? kAndGate : kOrGate;
      IGatePtr complement_gate;
      if (arg_gate->parents().size() == 1) {  // Optimization. Reuse.
        arg_gate->type(complement_type);
        arg_gate->InvertArgs();
        complement_gate = arg_gate;
      } else {
        complement_gate = IGatePtr(new IGate(complement_type));
        complement_gate->CopyArgs(arg_gate);
        complement_gate->InvertArgs();
      }
      gate_complements->insert(std::make_pair(arg_gate->index(),
                                              complement_gate));
      arg_gate = complement_gate;  // Needed for further propagation.
    }
    Preprocessor::PropagateComplements(arg_gate, gate_complements);
  }

  std::vector<int>::iterator it_ch;
  for (it_ch = to_swap.begin(); it_ch != to_swap.end(); ++it_ch) {
    assert(*it_ch < 0);
    gate->EraseArg(*it_ch);
    IGatePtr complement = gate_complements->find(-*it_ch)->second;
    bool ret = gate->AddArg(complement->index(), complement);
    assert(ret);  // No duplicates.
  }
}

bool Preprocessor::RemoveNullGates() {
  Preprocessor::ClearGateMarks();
  assert(null_gates_.empty());
  IGatePtr root = graph_->root();
  Preprocessor::GatherNullGates(root);
  Preprocessor::ClearGateMarks();
  if (null_gates_.size() == 1 && null_gates_.front().lock() == root)
    null_gates_.clear();  // Special case of only one NULL gate as the root.

  if (!null_gates_.empty()) {
    Preprocessor::ClearNullGates();
    return true;
  }
  return false;
}

void Preprocessor::GatherNullGates(const IGatePtr& gate) {
  if (gate->mark()) return;
  gate->mark(true);
  if (gate->type() == kNullGate && gate->state() == kNormalState) {
    null_gates_.push_back(gate);
  }
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    Preprocessor::GatherNullGates(it->second);
  }
}

bool Preprocessor::JoinGates(const IGatePtr& gate) {
  if (gate->mark()) return false;
  gate->mark(true);
  bool possible = false;  // If joining is possible at all.
  Operator target_type;  // What kind of arg gate are we searching for?
  switch (gate->type()) {
    case kNandGate:
    case kAndGate:
      //assert(gate->args().size() > 1);  /// @todo Put back.
      target_type = kAndGate;
      possible = true;
      break;
    case kNorGate:
    case kOrGate:
      //assert(gate->args().size() > 1);  /// @todo Put back.
      target_type = kOrGate;
      possible = true;
      break;
  }
  //assert(!gate->args().empty());  /// @todo Put back.
  std::vector<IGatePtr> to_join;  // Gate arguments of the same logic.
  bool changed = false;  // Indication if the tree is changed.
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    bool ret = false;  // Indication if the sub-tree has changed.
    IGatePtr arg_gate = it->second;
    ret = Preprocessor::JoinGates(arg_gate);
    if (!changed && ret) changed = true;

    if (!possible) continue;  // Joining with the parent is impossible.

    if (it->first < 0) continue;  // Cannot join a negative arg gate.
    if (arg_gate->IsModule()) continue;  // Does not coalesce modules.

    if (arg_gate->type() == target_type) to_join.push_back(arg_gate);
  }

  if (!changed && !to_join.empty()) changed = true;
  std::vector<IGatePtr>::iterator it_ch;
  for (it_ch = to_join.begin(); it_ch != to_join.end(); ++it_ch) {
    gate->JoinGate(*it_ch);
    if (gate->state() != kNormalState) {
      const_gates_.push_back(gate);  // Register for future processing.
      return true;  // The parent is constant. No need to join other arguments.
    }
    //assert(gate->args().size() > 1);  // Does not produce NULL type gates.
  }
  return changed;
}

void Preprocessor::DetectModules() {
  // First stage, traverse the tree depth-first for gates and indicate
  // visit time for each node.
  LOG(DEBUG2) << "Detecting modules...";

  Preprocessor::ClearNodeVisits();

  IGatePtr root_gate = graph_->root();
  int time = 0;
  Preprocessor::AssignTiming(time, root_gate);

  LOG(DEBUG3) << "Timings are assigned to nodes.";

  Preprocessor::ClearGateMarks();
  Preprocessor::FindModules(root_gate);

  assert(!root_gate->Revisited());
  assert(root_gate->min_time() == 1);
  assert(root_gate->max_time() == root_gate->ExitTime());
}

int Preprocessor::AssignTiming(int time, const IGatePtr& gate) {
  if (gate->Visit(++time)) return time;  // Revisited gate.
  assert(gate->constant_args().empty());

  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    time = Preprocessor::AssignTiming(time, it->second);
  }

  boost::unordered_map<int, VariablePtr>::const_iterator it_b;
  for (it_b = gate->variable_args().begin();
       it_b != gate->variable_args().end(); ++it_b) {
    it_b->second->Visit(++time);  // Enter the leaf.
    it_b->second->Visit(time);  // Exit at the same time.
  }
  bool re_visited = gate->Visit(++time);  // Exiting the gate in second visit.
  assert(!re_visited);  // No cyclic visiting.
  return time;
}

void Preprocessor::FindModules(const IGatePtr& gate) {
  if (gate->mark()) return;
  gate->mark(true);
  int enter_time = gate->EnterTime();
  int exit_time = gate->ExitTime();
  int min_time = enter_time;
  int max_time = exit_time;

  std::vector<std::pair<int, NodePtr> > non_shared_args;
  std::vector<std::pair<int, NodePtr> > modular_args;
  std::vector<std::pair<int, NodePtr> > non_modular_args;
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    IGatePtr arg_gate = it->second;
    Preprocessor::FindModules(arg_gate);
    if (arg_gate->IsModule() && !arg_gate->Revisited()) {
      assert(arg_gate->parents().size() == 1);
      assert(arg_gate->parents().count(gate->index()));

      non_shared_args.push_back(*it);
      continue;  // Sub-tree's visit times are within the Enter and Exit time.
    }
    int min = arg_gate->min_time();
    int max = arg_gate->max_time();
    assert(min > 0);
    assert(max > 0);
    assert(max > min);
    if (min > enter_time && max < exit_time) {
      modular_args.push_back(*it);
    } else {
      non_modular_args.push_back(*it);
    }
    min_time = std::min(min_time, min);
    max_time = std::max(max_time, max);
  }

  boost::unordered_map<int, VariablePtr>::const_iterator it_b;
  for (it_b = gate->variable_args().begin();
       it_b != gate->variable_args().end(); ++it_b) {
    VariablePtr arg = it_b->second;
    int min = arg->EnterTime();
    int max = arg->LastVisit();
    assert(min > 0);
    assert(max > 0);
    if (min == max) {
      assert(min > enter_time && max < exit_time);
      assert(arg->parents().size() == 1);
      assert(arg->parents().count(gate->index()));

      non_shared_args.push_back(*it_b);
      continue;  // The single parent argument.
    }
    assert(max > min);
    if (min > enter_time && max < exit_time) {
      modular_args.push_back(*it_b);
    } else {
      non_modular_args.push_back(*it_b);
    }
    min_time = std::min(min_time, min);
    max_time = std::max(max_time, max);
  }

  // Determine if this gate is module itself.
  if (min_time == enter_time && max_time == exit_time) {
    LOG(DEBUG3) << "Found original module: " << gate->index();
    assert((modular_args.size() + non_shared_args.size()) ==
           gate->args().size());
    gate->TurnModule();
  }

  max_time = std::max(max_time, gate->LastVisit());
  gate->min_time(min_time);
  gate->max_time(max_time);

  // Attempting to create new modules for specific gate types.
  switch (gate->type()) {
    case kNorGate:
    case kOrGate:
    case kNandGate:
    case kAndGate:
      Preprocessor::CreateNewModule(gate, non_shared_args);

      Preprocessor::FilterModularArgs(&modular_args, &non_modular_args);
      assert(modular_args.size() != 1);  // One modular arg is non-shared.
      std::vector<std::vector<std::pair<int, NodePtr> > > groups;
      Preprocessor::GroupModularArgs(modular_args, &groups);
      Preprocessor::CreateNewModules(gate, modular_args, groups);
  }
}

boost::shared_ptr<IGate> Preprocessor::CreateNewModule(
    const IGatePtr& gate,
    const std::vector<std::pair<int, NodePtr> >& args) {
  IGatePtr module;  // Empty pointer as an indication of a failure.
  if (args.empty()) return module;
  if (args.size() == 1) return module;
  if (args.size() == gate->args().size()) {
    assert(gate->IsModule());
    return module;
  }
  assert(args.size() < gate->args().size());
  switch (gate->type()) {
    case kNandGate:
    case kAndGate:
      module = IGatePtr(new IGate(kAndGate));
      break;
    case kNorGate:
    case kOrGate:
      module = IGatePtr(new IGate(kOrGate));
      break;
    default:
      return module;  // Cannot create sub-modules for other types.
  }
  module->TurnModule();
  module->mark(true);
  std::vector<std::pair<int, NodePtr> >::const_iterator it;
  for (it = args.begin(); it != args.end(); ++it) {
    gate->TransferArg(it->first, module);
  }
  gate->AddArg(module->index(), module);
  assert(gate->args().size() > 1);
  LOG(DEBUG3) << "Created a new module for Gate " << gate->index() << ": "
      << "Gate " << module->index() << " with "  << args.size()
      << " NON-SHARED arguments.";
  return module;
}

void Preprocessor::FilterModularArgs(
    std::vector<std::pair<int, NodePtr> >* modular_args,
    std::vector<std::pair<int, NodePtr> >* non_modular_args) {
  if (modular_args->empty() || non_modular_args->empty()) return;
  std::vector<std::pair<int, NodePtr> > new_non_modular;
  std::vector<std::pair<int, NodePtr> > still_modular;
  std::vector<std::pair<int, NodePtr> >::iterator it;
  for (it = modular_args->begin(); it != modular_args->end(); ++it) {
    int min = it->second->min_time();
    int max = it->second->max_time();
    bool non_module = false;
    std::vector<std::pair<int, NodePtr> >::iterator it_n;
    for (it_n = non_modular_args->begin();
         it_n != non_modular_args->end(); ++it_n) {
      int lower = it_n->second->min_time();
      int upper = it_n->second->max_time();
      int a = std::max(min, lower);
      int b = std::min(max, upper);
      if (a <= b) {  // There's some overlap between the ranges.
        non_module = true;
        break;
      }
    }
    if (non_module) {
      new_non_modular.push_back(*it);
    } else{
      still_modular.push_back(*it);
    }
  }
  Preprocessor::FilterModularArgs(&still_modular, &new_non_modular);
  *modular_args = still_modular;
  non_modular_args->insert(non_modular_args->end(), new_non_modular.begin(),
                           new_non_modular.end());
}

void Preprocessor::GroupModularArgs(
    const std::vector<std::pair<int, NodePtr> >& modular_args,
    std::vector<std::vector<std::pair<int, NodePtr> > >* groups) {
  if (modular_args.empty()) return;
  assert(modular_args.size() > 1);
  std::vector<std::pair<int, NodePtr> > to_check(modular_args);
  while (!to_check.empty()) {
    std::vector<std::pair<int, NodePtr> > group;
    NodePtr first_member = to_check.back().second;
    group.push_back(to_check.back());
    to_check.pop_back();
    int low = first_member->min_time();
    int high = first_member->max_time();

    int prev_size = 0;
    std::vector<std::pair<int, NodePtr> > next_check;
    while (prev_size < group.size()) {
      prev_size = group.size();
      std::vector<std::pair<int, NodePtr> >::iterator it;
      for (it = to_check.begin(); it != to_check.end(); ++it) {
        int min = it->second->min_time();
        int max = it->second->max_time();
        int a = std::max(min, low);
        int b = std::min(max, high);
        if (a <= b) {  // There's some overlap between the ranges.
          group.push_back(*it);
          low = std::min(min, low);
          high = std::max(max, high);
        } else {
          next_check.push_back(*it);
        }
      }
      to_check = next_check;
    }
    assert(group.size() > 1);
    groups->push_back(group);
  }
}

void Preprocessor::CreateNewModules(
    const IGatePtr& gate,
    const std::vector<std::pair<int, NodePtr> >& modular_args,
    const std::vector<std::vector<std::pair<int, NodePtr> > >& groups) {
  if (modular_args.empty()) return;
  assert(modular_args.size() > 1);
  assert(!groups.empty());
  if (modular_args.size() == gate->args().size() && groups.size() == 1) {
    assert(gate->IsModule());
    return;
  }
  IGatePtr main_arg;

  if (modular_args.size() == gate->args().size()) {
    assert(groups.size() > 1);
    assert(gate->IsModule());
    main_arg = gate;
  } else {
    main_arg = Preprocessor::CreateNewModule(gate, modular_args);
    assert(main_arg);
  }
  std::vector<std::vector<std::pair<int, NodePtr> > >::const_iterator it;
  for (it = groups.begin(); it != groups.end(); ++it) {
    Preprocessor::CreateNewModule(main_arg, *it);
  }
}

void Preprocessor::BooleanOptimization() {
  Preprocessor::ClearNodeVisits();
  Preprocessor::ClearGateMarks();

  std::vector<boost::weak_ptr<IGate> > common_gates;
  std::vector<boost::weak_ptr<Variable> > common_variables;
  Preprocessor::GatherCommonNodes(&common_gates, &common_variables);

  Preprocessor::ClearNodeVisits();
  std::vector<boost::weak_ptr<IGate> >::iterator it;
  for (it = common_gates.begin(); it != common_gates.end(); ++it) {
    Preprocessor::ProcessCommonNode(*it);
  }

  std::vector<boost::weak_ptr<Variable> >::iterator it_b;
  for (it_b = common_variables.begin(); it_b != common_variables.end();
       ++it_b) {
    Preprocessor::ProcessCommonNode(*it_b);
  }
}

void Preprocessor::GatherCommonNodes(
      std::vector<boost::weak_ptr<IGate> >* common_gates,
      std::vector<boost::weak_ptr<Variable> >* common_variables) {
  std::queue<IGatePtr> gates_queue;
  gates_queue.push(graph_->root());
  while (!gates_queue.empty()) {
    IGatePtr gate = gates_queue.front();
    gates_queue.pop();
    boost::unordered_map<int, IGatePtr>::const_iterator it;
    for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
      IGatePtr arg_gate = it->second;
      assert(arg_gate->state() == kNormalState);
      if (arg_gate->Visited()) continue;
      arg_gate->Visit(1);
      gates_queue.push(arg_gate);
      if (arg_gate->parents().size() > 1) common_gates->push_back(arg_gate);
    }

    boost::unordered_map<int, VariablePtr>::const_iterator it_b;
    for (it_b = gate->variable_args().begin();
         it_b != gate->variable_args().end(); ++it_b) {
      VariablePtr arg = it_b->second;
      if (arg->Visited()) continue;
      arg->Visit(1);
      if (arg->parents().size() > 1) common_variables->push_back(arg);
    }
  }
}

template<class N>
void Preprocessor::ProcessCommonNode(const boost::weak_ptr<N>& common_node) {
  if (common_node.expired()) return;  // The node has been deleted.

  boost::shared_ptr<N> node = common_node.lock();

  if (node->parents().size() == 1) return;  // The parent is deleted.

  IGatePtr root = graph_->root();
  Preprocessor::ClearOptiValues(root);

  assert(node->opti_value() == 0);
  node->opti_value(1);
  int mult_tot = node->parents().size();  // Total multiplicity.
  assert(mult_tot > 1);
  mult_tot += Preprocessor::PropagateFailure(node);
  // The results of the failure propagation.
  std::map<int, boost::weak_ptr<IGate> > destinations;
  int num_dest = 0;  // This is not the same as the size of destinations.
  if (root->opti_value() == 1) {  // The root gate failed.
    destinations.insert(std::make_pair(root->index(), root));
    num_dest = 1;
  } else {
    assert(root->opti_value() == 0);
    num_dest = Preprocessor::CollectFailureDestinations(root, node->index(),
                                                        &destinations);
  }

  if (num_dest == 0) return;  // No failure destination detected.
  assert(!destinations.empty());
  if (num_dest < mult_tot) {  // Redundancy detection.
    bool created_constant =
        Preprocessor::ProcessRedundantParents(node, &destinations);
    Preprocessor::ProcessFailureDestinations(node, destinations);
    if (created_constant) {
      Preprocessor::ClearGateMarks();
      Preprocessor::PropagateConstants(root);
      Preprocessor::ClearGateMarks();
      Preprocessor::RemoveNullGates();
    }
  }
}

int Preprocessor::PropagateFailure(const NodePtr& node) {
  assert(node->opti_value() == 1);
  int mult_tot = 0;
  boost::unordered_map<int, boost::weak_ptr<IGate> >::const_iterator it;
  for (it = node->parents().begin(); it != node->parents().end(); ++it) {
    assert(!it->second.expired());
    IGatePtr parent = it->second.lock();
    if (parent->opti_value() == 1) continue;
    parent->ArgFailed();  // Send a notification.
    if (parent->opti_value() == 1) {  // The parent failed.
      int mult = parent->parents().size();  // Multiplicity of the parent.
      if (mult > 1) mult_tot += mult;  // Total multiplicity.
      mult_tot += Preprocessor::PropagateFailure(parent);
    }
  }
  return mult_tot;
}

int Preprocessor::CollectFailureDestinations(
    const IGatePtr& gate,
    int index,
    std::map<int, boost::weak_ptr<IGate> >* destinations) {
  assert(gate->opti_value() == 0);
  if (gate->args().count(index)) {  // Argument may be non-gate.
    gate->opti_value(3);
  } else {
    gate->opti_value(2);
  }
  int num_dest = 0;
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    IGatePtr arg = it->second;
    if (arg->opti_value() == 0) {
      num_dest +=
          Preprocessor::CollectFailureDestinations(arg, index, destinations);
    } else if (arg->opti_value() == 1 && arg->index() != index) {
      ++num_dest;
      destinations->insert(std::make_pair(arg->index(), arg));
    } // Ignore gates with optimization values of 2 or 3.
  }
  return num_dest;
}

bool Preprocessor::ProcessRedundantParents(
    const NodePtr& node,
    std::map<int, boost::weak_ptr<IGate> >* destinations) {
  std::vector<boost::weak_ptr<IGate> > redundant_parents;
  boost::unordered_map<int, boost::weak_ptr<IGate> >::const_iterator it;
  for (it = node->parents().begin(); it != node->parents().end(); ++it) {
    assert(!it->second.expired());
    IGatePtr parent = it->second.lock();
    if (parent->opti_value() < 3) {
      // Special cases for the redundant parent and the destination parent.
      switch (parent->type()) {
        case kOrGate:
          if (destinations->count(parent->index())) {
            destinations->erase(parent->index());
            continue;  // No need to add into the redundancy list.
          }
      }
      redundant_parents.push_back(parent);
    }
  }
  // The node behaves like a constant False for redundant parents.
  bool created_constant = false;  // Parents turned into constants.
  std::vector<boost::weak_ptr<IGate> >::iterator it_r;
  for (it_r = redundant_parents.begin(); it_r != redundant_parents.end();
       ++it_r) {
    if (it_r->expired()) continue;
    IGatePtr parent = it_r->lock();
    switch (parent->type()) {
      case kAndGate:
        parent->Nullify();
        if (!created_constant) created_constant = true;
        break;
      case kOrGate:
        assert(parent->args().size() > 1);
        parent->EraseArg(node->index());
        if (parent->args().size() == 1) parent->type(kNullGate);
        break;
      case kAtleastGate:
        assert(parent->args().size() > 2);
        parent->EraseArg(node->index());
        if (parent->args().size() == parent->vote_number())
          parent->type(kAndGate);
        break;
      default:
        assert(false);
    }
  }
  return created_constant;
}

template<class N>
void Preprocessor::ProcessFailureDestinations(
    const boost::shared_ptr<N>& node,
    const std::map<int, boost::weak_ptr<IGate> >& destinations) {
  std::map<int, boost::weak_ptr<IGate> >::const_iterator it_d;
  for (it_d = destinations.begin(); it_d != destinations.end(); ++it_d) {
    if (it_d->second.expired()) continue;
    IGatePtr target = it_d->second.lock();
    assert(target->type() != kNullGate);
    switch (target->type()) {
      case kOrGate:
        target->AddArg(node->index(), node);
        break;
      case kAndGate:
      case kAtleastGate:
        IGatePtr new_gate(new IGate(target->type()));
        new_gate->vote_number(target->vote_number());
        new_gate->CopyArgs(target);
        target->EraseAllArgs();
        target->type(kOrGate);
        target->AddArg(new_gate->index(), new_gate);
        target->AddArg(node->index(), node);
        break;
    }
  }
}

bool Preprocessor::ProcessMultipleDefinitions() {
  assert(null_gates_.empty());
  assert(const_gates_.empty());
  // The original gate and its multiple definitions.
  boost::unordered_map<IGatePtr, std::vector<boost::weak_ptr<IGate> > >
      multi_def;
  std::vector<std::vector<IGatePtr> > orig_gates(kNumOperators,
                                                 std::vector<IGatePtr>());
  Preprocessor::ClearGateMarks();
  IGatePtr root = graph_->root();
  Preprocessor::DetectMultipleDefinitions(root, &multi_def, &orig_gates);

  if (multi_def.empty()) return false;
  boost::unordered_map<IGatePtr,
      std::vector<boost::weak_ptr<IGate> > >::iterator it;
  for (it = multi_def.begin(); it != multi_def.end(); ++it) {
    IGatePtr orig_gate = it->first;
    std::vector<boost::weak_ptr<IGate> >& duplicates = it->second;
    std::vector<boost::weak_ptr<IGate> >::iterator it_dup;
    for (it_dup = duplicates.begin(); it_dup != duplicates.end(); ++it_dup) {
      if (it_dup->expired()) continue;
      IGatePtr dup = it_dup->lock();
      // Swap this gate with the original gate because it is redefined.
      int index = dup->index();
      boost::unordered_map<int, boost::weak_ptr<IGate> > parents =
          dup->parents();  // Copy because parents will be deleted.
      boost::unordered_map<int, boost::weak_ptr<IGate> >::iterator it_parent;
      for (it_parent = parents.begin(); it_parent != parents.end();
           ++it_parent) {
        if (it_parent->second.expired()) continue;
        IGatePtr parent = it_parent->second.lock();
        int sign = 1;  // Guessing the sign.
        if (parent->args().count(-index)) sign = -1;
        parent->EraseArg(sign * index);
        parent->AddArg(sign * orig_gate->index(), orig_gate);

        if (parent->state() != kNormalState) {
          const_gates_.push_back(parent);
        } else if (parent->type() == kNullGate) {
          null_gates_.push_back(parent);
        }
      }
    }
  }
  if (!const_gates_.empty()) Preprocessor::ClearConstGates();
  if (!null_gates_.empty()) Preprocessor::ClearNullGates();
  return true;
}

void Preprocessor::DetectMultipleDefinitions(
    const IGatePtr& gate,
    boost::unordered_map<IGatePtr,
                         std::vector<boost::weak_ptr<IGate> > >* multi_def,
    std::vector<std::vector<IGatePtr> >* gates) {
  if (gate->mark()) return;
  gate->mark(true);
  assert(gate->state() == kNormalState);

  Operator type = gate->type();
  std::vector<IGatePtr>& type_group = (*gates)[type];
  std::vector<IGatePtr>::iterator it;
  for (it = type_group.begin(); it != type_group.end(); ++it) {
    IGatePtr orig_gate = *it;
    assert(orig_gate->mark());
    if (orig_gate->args() == gate->args()) {
      // This might be multiple definition. Extra check for K/N gates.
      if (type == kAtleastGate &&
          orig_gate->vote_number() != gate->vote_number()) continue;  // No.
      // Register this gate for replacement.
      if (multi_def->count(orig_gate)) {
        multi_def->find(orig_gate)->second.push_back(gate);
      } else {
        std::vector<boost::weak_ptr<IGate> > duplicates(1, gate);
        multi_def->insert(std::make_pair(orig_gate, duplicates));
      }
      return;
    }
  }
  // No redefinition is found for this gate. In order to avoid a comparison
  // with descendants, this gate is not yet put into original gates container.
  boost::unordered_map<int, IGatePtr>::const_iterator it_ch;
  for (it_ch = gate->gate_args().begin(); it_ch != gate->gate_args().end();
       ++it_ch) {
    Preprocessor::DetectMultipleDefinitions(it_ch->second, multi_def, gates);
  }
  type_group.push_back(gate);
}

void Preprocessor::ClearGateMarks() {
  Preprocessor::ClearGateMarks(graph_->root());
}

void Preprocessor::ClearGateMarks(const IGatePtr& gate) {
  if (!gate->mark()) return;
  gate->mark(false);
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    Preprocessor::ClearGateMarks(it->second);
  }
}

void Preprocessor::ClearNodeVisits() {
  Preprocessor::ClearNodeVisits(graph_->root());
}

void Preprocessor::ClearNodeVisits(const IGatePtr& gate) {
  gate->ClearVisits();
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    Preprocessor::ClearNodeVisits(it->second);
  }
  boost::unordered_map<int, VariablePtr>::const_iterator it_b;
  for (it_b = gate->variable_args().begin();
       it_b != gate->variable_args().end(); ++it_b) {
    it_b->second->ClearVisits();
  }
  boost::unordered_map<int, ConstantPtr>::const_iterator it_c;
  for (it_c = gate->constant_args().begin();
       it_c != gate->constant_args().end(); ++it_c) {
    it_c->second->ClearVisits();
  }
}

void Preprocessor::ClearOptiValues(const IGatePtr& gate) {
  gate->opti_value(0);
  gate->ResetArgFailure();
  boost::unordered_map<int, IGatePtr>::const_iterator it;
  for (it = gate->gate_args().begin(); it != gate->gate_args().end(); ++it) {
    Preprocessor::ClearOptiValues(it->second);
  }
  boost::unordered_map<int, VariablePtr>::const_iterator it_b;
  for (it_b = gate->variable_args().begin();
       it_b != gate->variable_args().end(); ++it_b) {
    it_b->second->opti_value(0);
  }
  assert(gate->constant_args().empty());
}

}  // namespace scram
