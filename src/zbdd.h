/*
 * Copyright (C) 2015 Olzhas Rakhimov
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

/// @file zbdd.h
/// Zero-Suppressed Binary Decision Diagram facilities.

#ifndef SCRAM_SRC_ZBDD_H_
#define SCRAM_SRC_ZBDD_H_

#include <cstdint>
#include <utility>
#include <vector>

#include <boost/unordered_map.hpp>

#include "bdd.h"

namespace scram {

/// @class SetNode
/// Representation of non-terminal nodes in ZBDD.
class SetNode : public NonTerminal {
 public:
  using NonTerminal::NonTerminal;  ///< Constructor with index and order.

  /// @returns Whatever count is stored in this node.
  int64_t count() const { return count_; }

  /// Stores numerical value for later retrieval.
  /// This is a helper functionality
  /// for counting the number of sets or nodes.
  ///
  /// @param[in] number  A number with a meaning for the caller.
  void count(int64_t number) { count_ = number; }

  /// @returns Cut sets found in the ZBDD represented by this node.
  const std::vector<std::vector<int>>& cut_sets() const { return cut_sets_; }

  /// Sets the cut sets belonging to this ZBDD.
  ///
  /// @param[in] cut_sets  Cut sets calculated from low and high edges.
  void cut_sets(const std::vector<std::vector<int>>& cut_sets) {
    cut_sets_ = cut_sets;
  }

  /// Recovers a shared pointer to SetNode from a pointer to Vertex.
  ///
  /// @param[in] vertex  Pointer to a Vertex known to be a SetNode.
  ///
  /// @return Casted pointer to SetNode.
  static std::shared_ptr<SetNode> Ptr(const std::shared_ptr<Vertex>& vertex) {
    return std::static_pointer_cast<SetNode>(vertex);
  }

 private:
  std::vector<std::vector<int>> cut_sets_;  ///< Cut sets of this node.
  int64_t count_ = 0;  ///< The number of cut sets, nodes, or anything else.
};

/// @class Zbdd
/// Zero-Suppressed Binary Decision Diagrams for set manipulations.
class Zbdd {
 public:
  /// Converts Reduced Ordered BDD
  /// into Zero-Suppressed BDD.
  ///
  /// @param[in] bdd  ROBDD with the ITE vertices.
  /// @param[in] settings  Settings for analysis.
  ///
  /// @pre Boolean graph is coherent (monotonic).
  /// @pre BDD has attributed edges with only one terminal (1/True).
  Zbdd(const Bdd* bdd, const Settings& settings) noexcept;

  /// Constructor with the analysis target.
  /// ZBDD is directly produced from a Boolean graph.
  ///
  /// @param[in] fault_tree  Preprocessed, partially normalized,
  ///                        and indexed fault tree.
  /// @param[in] settings  The analysis settings.
  ///
  /// @note The passed Boolean graph must already have variable ordering.
  /// @note The construction may take considerable time.
  Zbdd(const BooleanGraph* fault_tree, const Settings& settings) noexcept;

  /// Runs the analysis
  /// with the representation of a Boolean graph
  /// as ZBDD.
  ///
  /// @pre Only coherent analysis is requested.
  void Analyze() noexcept;

  /// @returns Cut sets generated by the analysis.
  const std::vector<std::vector<int>>& cut_sets() const { return cut_sets_; }

 private:
  using NodePtr = std::shared_ptr<Node>;
  using VariablePtr = std::shared_ptr<Variable>;
  using IGatePtr = std::shared_ptr<IGate>;
  using VertexPtr = std::shared_ptr<Vertex>;
  using TerminalPtr = std::shared_ptr<Terminal>;
  using ItePtr = std::shared_ptr<Ite>;
  using SetNodePtr = std::shared_ptr<SetNode>;
  using UniqueTable = TripletTable<SetNodePtr>;
  using ComputeTable = TripletTable<VertexPtr>;
  using PairTable = boost::unordered_map<std::pair<int, int>, VertexPtr>;
  using CutSet = std::vector<int>;

  /// Default constructor to initialize member variables.
  ///
  /// @param[in] settings  Settings that control analysis complexity.
  explicit Zbdd(const Settings& settings) noexcept;

  /// Converts BDD graph into ZBDD graph.
  ///
  /// @param[in] vertex  Vertex of the ROBDD graph.
  /// @param[in] complement  Interpretation of the vertex as complement.
  /// @param[in] bdd_graph  The main ROBDD as helper database.
  /// @param[in] limit_order  The maximum size of requested sets.
  ///
  /// @returns Pointer to the root vertex of the ZBDD graph.
  VertexPtr ConvertBdd(const VertexPtr& vertex, bool complement,
                       const Bdd* bdd_graph, int limit_order) noexcept;

  /// Transforms a Boolean graph gate into a Zbdd set graph.
  ///
  /// @param[in] gate  The root gate of the Boolean graph.
  ///
  /// @returns The top vertex of the Zbdd graph.
  VertexPtr ConvertGraph(const IGatePtr& gate) noexcept;

  /// Creates a Zbdd vertex from a Boolean variable.
  ///
  /// @param[in] variable  Boolean graph variable.
  ///
  /// @returns Pointer to the root vertex of the Zbdd graph.
  SetNodePtr ConvertGraph(const VariablePtr& variable) noexcept;

  /// Creates a vertex to represent a module gate.
  ///
  /// @param[in] gate  The root or current parent gate of the graph.
  ///
  /// @returns Pointer to the ZBDD set vertex.
  ///
  /// @note The gate still needs to be converted and saved.
  SetNodePtr CreateModuleProxy(const IGatePtr& gate) noexcept;

  /// Applies Boolean operation to two vertices representing sets.
  ///
  /// @param[in] type  The operator or type of the gate.
  /// @param[in] arg_one  First argument ZBDD set.
  /// @param[in] arg_two  Second argument ZBDD set.
  VertexPtr Apply(Operator type, const VertexPtr& arg_one,
                  const VertexPtr& arg_two) noexcept;

  /// Applies the logic of a Boolean operator
  /// to terminal vertices.
  ///
  /// @param[in] type  The operator to apply.
  /// @param[in] term_one  First argument terminal vertex.
  /// @param[in] term_two  Second argument terminal vertex.
  ///
  /// @returns The resulting ZBDD vertex.
  VertexPtr Apply(Operator type, const TerminalPtr& term_one,
                  const TerminalPtr& term_two) noexcept;

  /// Applies the logic of a Boolean operator
  /// to non-terminal and terminal vertices.
  ///
  /// @param[in] type  The operator or type of the gate.
  /// @param[in] set_node  Non-terminal vertex.
  /// @param[in] term  Terminal vertex.
  ///
  /// @returns The resulting ZBDD vertex.
  VertexPtr Apply(Operator type, const SetNodePtr& set_node,
                  const TerminalPtr& term) noexcept;

  /// Applies Boolean operation to ZBDD graph non-terminal vertices.
  ///
  /// @param[in] type  The operator or type of the gate.
  /// @param[in] arg_one  First argument set vertex.
  /// @param[in] arg_two  Second argument set vertex.
  ///
  /// @returns The resulting ZBDD vertex.
  ///
  /// @pre Argument vertices are ordered.
  VertexPtr Apply(Operator type, const SetNodePtr& arg_one,
                  const SetNodePtr& arg_two) noexcept;

  /// Produces canonical signature of application of Boolean operations.
  /// The signature of the operations helps
  /// detect equivalent operations.
  ///
  /// @param[in] type  The operator or type of the gate.
  /// @param[in] arg_one  First argument vertex.
  /// @param[in] arg_two  Second argument vertex.
  ///
  /// @returns Unique signature of the operation.
  ///
  /// @pre The arguments are not the same functions.
  ///      Equal ID functions are handled by the reduction.
  /// @pre Even though the arguments are not SetNodePtr type,
  ///      they are ZBDD SetNode vertices.
  Triplet GetSignature(Operator type, const VertexPtr& arg_one,
                       const VertexPtr& arg_two) noexcept;

  /// Removes subsets in ZBDD.
  ///
  /// @param[in] vertex  The variable node in the set.
  ///
  /// @returns Processed vertex.
  VertexPtr Minimize(const VertexPtr& vertex) noexcept;

  /// Applies subsume operation on two sets.
  /// Subsume operation removes
  /// paths that exist in Low branch from High branch.
  ///
  /// @param[in] high  True/then/high branch of a variable.
  /// @param[in] low  False/else/low branch of a variable.
  ///
  /// @returns Minimized high branch for a variable.
  VertexPtr Subsume(const VertexPtr& high, const VertexPtr& low) noexcept;

  /// Traverses the reduced ZBDD graph to generate cut sets.
  ///
  /// @param[in] vertex  The root node in traversal.
  ///
  /// @returns A collection of cut sets
  ///          generated from the ZBDD subgraph.
  std::vector<std::vector<int>>
  GenerateCutSets(const VertexPtr& vertex) noexcept;

  /// Counts the number of SetNodes.
  ///
  /// @param[in] vertex  The root vertex to start counting.
  ///
  /// @returns The total number of SetNode vertices
  ///          including vertices in modules.
  ///
  /// @pre SetNode marks are clear (false).
  int CountSetNodes(const VertexPtr& vertex) noexcept;

  /// Counts the total number of sets in ZBDD.
  ///
  /// @param[in] vertex  The root vertex of ZBDD.
  ///
  /// @returns The number of cut sets in ZBDD.
  ///
  /// @pre SetNode marks are clear (false).
  int64_t CountCutSets(const VertexPtr& vertex) noexcept;

  /// Cleans up non-terminal vertex marks
  /// by setting them to "false".
  ///
  /// @param[in] vertex  The root vertex of the graph.
  ///
  /// @pre The graph is marked "true" contiguously.
  void ClearMarks(const VertexPtr& vertex) noexcept;

  /// Table of unique SetNodes denoting sets.
  /// The key consists of (index, id_high, id_low) triplet.
  UniqueTable unique_table_;

  /// Table of processed computations over sets.
  /// The key must convey the semantics of the operation over functions.
  /// The argument functions are recorded with their IDs (not vertex indices).
  /// In order to keep only unique computations,
  /// the argument IDs must be ordered.
  ComputeTable compute_table_;

  /// The results of subsume operations over sets.
  PairTable subsume_table_;

  const Settings kSettings_;  ///< Analysis settings.
  VertexPtr root_;  ///< The root vertex of ZBDD.
  /// Processed function graphs with ids and limit order.
  boost::unordered_map<std::pair<int, int>, VertexPtr> ites_;
  std::unordered_map<int, VertexPtr> gates_;  ///< Processed gates.
  std::unordered_map<int, VertexPtr> modules_;  ///< Module graphs.
  const TerminalPtr kBase_;  ///< Terminal Base (Unity/1) set.
  const TerminalPtr kEmpty_;  ///< Terminal Empty (Null/0) set.
  int set_id_;  ///< Identification assignment for new set graphs.
  std::unordered_map<int, VertexPtr> minimal_results_;  ///< Memorize minimal.
  std::vector<CutSet> cut_sets_;  ///< Generated cut sets.
};

}  // namespace scram

#endif  // SCRAM_SRC_ZBDD_H_
