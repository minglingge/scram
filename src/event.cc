/*
 * Copyright (C) 2014-2017 Olzhas Rakhimov
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

/// @file event.cc
/// Implementation of Event Class and its derived classes.

#include "event.h"

#include <boost/range/algorithm.hpp>

#include "ext/algorithm.h"

namespace scram {
namespace mef {

Event::~Event() = default;

void Gate::Validate() const {
  // Detect inhibit flavor.
  if (formula_->type() != kAnd || !Element::HasAttribute("flavor") ||
      Element::GetAttribute("flavor").value != "inhibit") {
    return;
  }
  if (formula_->num_args() != 2) {
    throw ValidationError(Element::name() +
                          "INHIBIT gate must have only 2 children");
  }
  int num_conditional = boost::count_if(
      formula_->event_args(), [](const Formula::EventArg& event) {
        if (!boost::get<BasicEvent*>(&event))
          return false;
        auto& basic_event = boost::get<BasicEvent*>(event);
        return basic_event->HasAttribute("flavor") &&
               basic_event->GetAttribute("flavor").value == "conditional";
      });
  if (num_conditional != 1)
    throw ValidationError(Element::name() + " : INHIBIT gate must have" +
                          " exactly one conditional event.");
}

Formula::Formula(Operator type) : type_(type), vote_number_(0) {}

int Formula::vote_number() const {
  if (!vote_number_)
    throw LogicError("Vote number is not set.");
  return vote_number_;
}

void Formula::vote_number(int number) {
  if (type_ != kVote) {
    throw LogicError(
        "The vote number can only be defined for 'atleast' formulas. "
        "The operator of this formula is '" +
        std::string(kOperatorToString[type_]) + "'.");
  }
  if (number < 2)
    throw InvalidArgument("Vote number cannot be less than 2.");
  if (vote_number_)
    throw LogicError("Trying to re-assign a vote number");

  vote_number_ = number;
}

void Formula::AddArgument(EventArg event_arg) {
  auto up_cast = [](const EventArg& var_arg) {
    return boost::apply_visitor([](auto* arg) -> Event* { return arg; },
                                var_arg);
  };
  Event* event = up_cast(event_arg);
  if (ext::any_of(event_args_, [&event, &up_cast](const EventArg& arg) {
        return up_cast(arg)->id() == event->id();
      })) {
    throw DuplicateArgumentError("Duplicate argument " + event->name());
  }
  event_args_.push_back(event_arg);
  if (event->orphan())
    event->orphan(false);
}

void Formula::Validate() const {
  switch (type_) {
    case kAnd:
    case kOr:
    case kNand:
    case kNor:
      if (num_args() < 2)
        throw ValidationError("\"" + std::string(kOperatorToString[type_]) +
                              "\" formula must have 2 or more arguments.");
      break;
    case kNot:
    case kNull:
      if (num_args() != 1)
        throw ValidationError("\"" + std::string(kOperatorToString[type_]) +
                              "\" formula must have only one argument.");
      break;
    case kXor:
      if (num_args() != 2)
        throw ValidationError("\"xor\" formula must have exactly 2 arguments.");
      break;
    case kVote:
      if (num_args() <= vote_number_)
        throw ValidationError("\"atleast\" formula must have more arguments "
                              "than its vote number " +
                              std::to_string(vote_number_) + ".");
  }
}

}  // namespace mef
}  // namespace scram
