#include "optimizer/join_order_optimizer.hpp"

#include "parser/expression/list.hpp"
#include "planner/operator/list.hpp"

#include <algorithm>

using namespace duckdb;
using namespace std;

using JoinNode = JoinOrderOptimizer::JoinNode;

//! Returns true if A and B are disjoint, false otherwise
template <class T> static bool Disjoint(unordered_set<T> &a, unordered_set<T> &b) {
	for (auto &entry : a) {
		if (b.find(entry) != b.end()) {
			return false;
		}
	}
	return true;
}

//! Extract the set of relations referred to inside an expression
bool JoinOrderOptimizer::ExtractBindings(Expression &expression, unordered_set<size_t> &bindings) {
	if (expression.type == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = (BoundColumnRefExpression &)expression;
		if (colref.depth > 0) {
			// correlated column reference, we don't allow this to be reshuffled inside the subquery
			// we clear any currently made bindings
			bindings.clear();
			return false;
		}
		assert(colref.binding.table_index != (uint32_t)-1);
		// map the base table index to the relation index used by the JoinOrderOptimizer
		assert(relation_mapping.find(colref.binding.table_index) != relation_mapping.end());
		bindings.insert(relation_mapping[colref.binding.table_index]);
	}
	if (expression.type == ExpressionType::BOUND_REF) {
		// bound expression, don't use it for reordering
		bindings.clear();
		return false;
	}
	if (expression.type == ExpressionType::SUBQUERY) {
		auto &subquery = (BoundSubqueryExpression &)expression;
		if (subquery.is_correlated) {
			// we don't allow correlated subqueries to be reordered
			// FIXME: we could extract all the correlated table_indexes referenced inside the subquery here
			bindings.clear();
			return false;
		}
	}
	bool can_reorder = true;
	expression.EnumerateChildren([&](Expression *expr) {
		if (!ExtractBindings(*expr, bindings)) {
			can_reorder = false;
			return;
		}
	});
	return can_reorder;
}

static void ExtractFilters(LogicalOperator *op, vector<unique_ptr<Expression>> &filters) {
	for (size_t i = 0; i < op->expressions.size(); i++) {
		filters.push_back(move(op->expressions[i]));
	}
	op->expressions.clear();
}

static void GetTableReferences(LogicalOperator *op, unordered_set<size_t> &bindings) {
	if (op->type == LogicalOperatorType::GET) {
		auto get = (LogicalGet *)op;
		bindings.insert(get->table_index);
	} else if (op->type == LogicalOperatorType::SUBQUERY) {
		auto subquery = (LogicalSubquery *)op;
		bindings.insert(subquery->table_index);
	} else if (op->type == LogicalOperatorType::TABLE_FUNCTION) {
		auto table_function = (LogicalTableFunction *)op;
		bindings.insert(table_function->table_index);
	} else {
		// iterate over the children
		for (auto &child : op->children) {
			GetTableReferences(child.get(), bindings);
		}
	}
}

static unique_ptr<LogicalOperator> PushFilter(unique_ptr<LogicalOperator> node, unique_ptr<Expression> expr) {
	// push an expression into a filter
	// first check if we have any filter to push it into
	if (node->type != LogicalOperatorType::FILTER) {
		// we don't, we need to create one
		auto filter = make_unique<LogicalFilter>();
		filter->children.push_back(move(node));
		node = move(filter);
	}
	// push the filter into the LogicalFilter
	assert(node->type == LogicalOperatorType::FILTER);
	auto filter = (LogicalFilter *)node.get();
	filter->expressions.push_back(move(expr));
	return node;
}

static JoinSide CombineJoinSide(JoinSide left, JoinSide right) {
	if (left == JoinSide::NONE) {
		return right;
	}
	if (right == JoinSide::NONE) {
		return left;
	}
	if (left != right) {
		return JoinSide::BOTH;
	}
	return left;
}

static JoinSide GetJoinSide(Expression &expression, unordered_set<size_t> &left_bindings,
                            unordered_set<size_t> &right_bindings) {
	if (expression.type == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = (BoundColumnRefExpression &)expression;
		if (colref.depth > 0) {
			// correlated column reference, we can't join on this
			return JoinSide::BOTH;
		}
		if (left_bindings.find(colref.binding.table_index) != left_bindings.end()) {
			// column references table on left side
			assert(right_bindings.find(colref.binding.table_index) == right_bindings.end());
			return JoinSide::LEFT;
		} else {
			// column references table on right side
			assert(right_bindings.find(colref.binding.table_index) != right_bindings.end());
			return JoinSide::RIGHT;
		}
	}
	if (expression.type == ExpressionType::BOUND_REF) {
		// column reference has already been bound, don't use it for reordering
		return JoinSide::NONE;
	}
	if (expression.type == ExpressionType::SUBQUERY) {
		return JoinSide::BOTH;
	}
	JoinSide join_side = JoinSide::NONE;
	expression.EnumerateChildren([&](Expression *child) {
		auto child_side = GetJoinSide(*child, left_bindings, right_bindings);
		join_side = CombineJoinSide(child_side, join_side);
	});
	return join_side;
}

static unique_ptr<LogicalOperator> CreateJoinCondition(unique_ptr<LogicalOperator> op, LogicalJoin &join,
                                                       unique_ptr<Expression> expr,
                                                       unordered_set<size_t> &left_bindings,
                                                       unordered_set<size_t> &right_bindings) {
	auto total_side = GetJoinSide(*expr, left_bindings, right_bindings);
	if (total_side != JoinSide::BOTH) {
		// join condition does not reference both sides, add it as filter under the join
		int push_side = total_side == JoinSide::LEFT ? 0 : 1;
		join.children[push_side] = PushFilter(move(join.children[push_side]), move(expr));
		return op;
	} else if (expr->type >= ExpressionType::COMPARE_EQUAL && expr->type <= ExpressionType::COMPARE_NOTLIKE) {
		// comparison
		auto &comparison = (ComparisonExpression &)*expr;
		auto left_side = GetJoinSide(*comparison.left, left_bindings, right_bindings);
		auto right_side = GetJoinSide(*comparison.right, left_bindings, right_bindings);
		if (left_side != JoinSide::BOTH && right_side != JoinSide::BOTH) {
			// join condition can be divided in a left/right side
			JoinCondition condition;
			condition.comparison = expr->type;
			auto left = move(comparison.left);
			auto right = move(comparison.right);
			if (left_side == JoinSide::RIGHT) {
				// left = right, right = left, flip the comparison symbol and reverse sides
				swap(left, right);
				condition.comparison = ComparisonExpression::FlipComparisionExpression(expr->type);
			}
			condition.left = move(left);
			condition.right = move(right);
			join.conditions.push_back(move(condition));
			return op;
		}
	} else if (expr->type == ExpressionType::OPERATOR_NOT) {
		auto &op_expr = (OperatorExpression &)*expr;
		assert(op_expr.children.size() == 1);
		ExpressionType child_type = op_expr.children[0]->GetExpressionType();

		// the condition is ON NOT (EXPRESSION)
		// we can transform this to remove the NOT if the child is a Comparison
		// e.g.:
		// ON NOT (X = 3) can be turned into ON (X <> 3)
		// ON NOT (X > 3) can be turned into ON (X <= 3)
		// for non-comparison operators here we just push the filter
		if (child_type >= ExpressionType::COMPARE_EQUAL && child_type <= ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
			// switcheroo the child condition
			// our join needs to compare explicit left and right sides. So we
			// invert the condition to express NOT, this way we can still use
			// equi-joins
			op_expr.children[0]->type = ComparisonExpression::NegateComparisionExpression(child_type);
			return CreateJoinCondition(move(op), join, move(op_expr.children[0]), left_bindings, right_bindings);
		}
	}
	// filter is on both sides of the join
	// but the type was not recognized
	// push as filter under the join
	op = PushFilter(move(op), move(expr));
	return op;
}

//! Resolve join conditions for non-inner joins
unique_ptr<LogicalOperator> JoinOrderOptimizer::ResolveJoinConditions(unique_ptr<LogicalOperator> op) {
	// first resolve the join conditions of any children
	for (size_t i = 0; i < op->children.size(); i++) {
		op->children[i] = ResolveJoinConditions(move(op->children[i]));
	}
	if (op->type == LogicalOperatorType::JOIN) {
		LogicalJoin &join = (LogicalJoin &)*op;
		if (join.expressions.size() > 0) {
			// turn any remaining expressions into proper join conditions
			unordered_set<size_t> left_bindings, right_bindings;
			GetTableReferences(join.children[0].get(), left_bindings);
			GetTableReferences(join.children[1].get(), right_bindings);
			// now for each expression turn it into a proper JoinCondition
			for (size_t i = 0; i < join.expressions.size(); i++) {
				op = CreateJoinCondition(move(op), join, move(join.expressions[i]), left_bindings, right_bindings);
			}
			join.expressions.clear();
		}
	}
	return op;
}

bool JoinOrderOptimizer::ExtractJoinRelations(LogicalOperator &input_op, vector<LogicalOperator *> &filter_operators,
                                              LogicalOperator *parent) {
	LogicalOperator *op = &input_op;
	while (op->children.size() == 1 && op->type != LogicalOperatorType::SUBQUERY) {
		if (op->type == LogicalOperatorType::FILTER) {
			// extract join conditions from filter
			filter_operators.push_back(op);
		}
		if (op->type == LogicalOperatorType::AGGREGATE_AND_GROUP_BY) {
			// don't push filters through aggregate and group by
			JoinOrderOptimizer optimizer;
			op->children[0] = optimizer.Optimize(move(op->children[0]));
			return false;
		}
		op = op->children[0].get();
	}
	if (op->type == LogicalOperatorType::UNION || op->type == LogicalOperatorType::EXCEPT ||
	    op->type == LogicalOperatorType::INTERSECT) {
		// set operation, optimize separately in children
		for (size_t i = 0; i < op->children.size(); i++) {
			JoinOrderOptimizer optimizer;
			op->children[i] = optimizer.Optimize(move(op->children[i]));
		}
		return false;
	}

	if (op->type == LogicalOperatorType::JOIN) {
		LogicalJoin *join = (LogicalJoin *)op;
		if (join->type != JoinType::INNER) {
			// non-inner join
			// we do not reorder non-inner joins yet, however we do want to expand the potential join graph around them
			// non-inner joins are also tricky because we can't freely make conditions through them
			// e.g. suppose we have (left LEFT OUTER JOIN right WHERE right IS NOT NULL), the join can generate
			// new NULL values in the right side, so pushing this condition through the join leads to incorrect results
			// for this reason, we just start a new JoinOptimizer pass in each of the children of the join
			JoinOrderOptimizer optimizer_left, optimizer_right;
			join->children[0] = optimizer_left.Optimize(move(join->children[0]));
			join->children[1] = optimizer_right.Optimize(move(join->children[1]));
			// after this we want to treat this node as one  "end node" (like e.g. a base relation)
			// however the join refers to multiple base relations
			// enumerate all base relations obtained from this join and add them to the relation mapping
			// also, we have to resolve the join conditions for the joins here
			// get the left and right bindings
			unordered_set<size_t> bindings;
			GetTableReferences(join, bindings);
			// now create the relation that refers to all these bindings
			auto relation = make_unique<Relation>(&input_op, parent);
			for (size_t it : bindings) {
				relation_mapping[it] = relations.size();
			}
			relations.push_back(move(relation));
			return true;
		} else {
			// extract join conditions from inner join
			filter_operators.push_back(op);
		}
	}
	if (op->type == LogicalOperatorType::JOIN || op->type == LogicalOperatorType::CROSS_PRODUCT) {
		// inner join or cross product
		if (!ExtractJoinRelations(*op->children[0], filter_operators, op)) {
			return false;
		}
		if (!ExtractJoinRelations(*op->children[1], filter_operators, op)) {
			return false;
		}
		return true;
	} else if (op->type == LogicalOperatorType::GET) {
		// base table scan, add to set of relations
		auto get = (LogicalGet *)op;
		auto relation = make_unique<Relation>(&input_op, parent);
		relation_mapping[get->table_index] = relations.size();
		relations.push_back(move(relation));
		return true;
	} else if (op->type == LogicalOperatorType::SUBQUERY) {
		auto subquery = (LogicalSubquery *)op;
		assert(op->children.size() == 1);
		// we run the join order optimizer witin the subquery as well
		JoinOrderOptimizer optimizer;
		op->children[0] = optimizer.Optimize(move(op->children[0]));
		// now we add the subquery to the set of relations
		auto relation = make_unique<Relation>(&input_op, parent);
		relation_mapping[subquery->table_index] = relations.size();
		relations.push_back(move(relation));
		return true;
	} else if (op->type == LogicalOperatorType::TABLE_FUNCTION) {
		// table function call, add to set of relations
		auto table_function = (LogicalTableFunction *)op;
		auto relation = make_unique<Relation>(&input_op, parent);
		relation_mapping[table_function->table_index] = relations.size();
		relations.push_back(move(relation));
		return true;
	}
	return false;
}

//! Update the exclusion set with all entries in the subgraph
static void UpdateExclusionSet(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	for (size_t i = 0; i < node->count; i++) {
		exclusion_set.insert(node->relations[i]);
	}
}

//! Create a new JoinTree node by joining together two previous JoinTree nodes
static unique_ptr<JoinNode> CreateJoinTree(RelationSet *set, NeighborInfo *info, JoinNode *left, JoinNode *right) {
	// for the hash join we want the right side (build side) to have the smallest cardinality
	// also just a heuristic but for now...
	// FIXME: we should probably actually benchmark that as well
	// FIXME: should consider different join algorithms, should we pick a join algorithm here as well? (probably)
	if (left->cardinality < right->cardinality) {
		return CreateJoinTree(set, info, right, left);
	}
	// the expected cardinality is the max of the child cardinalities
	// FIXME: we should obviously use better cardinality estimation here
	// but for now we just assume foreign key joins only
	size_t expected_cardinality;
	if (info->filters.size() == 0) {
		// cross product
		expected_cardinality = left->cardinality * right->cardinality;
	} else {
		// normal join, expect foreign key join
		expected_cardinality = std::max(left->cardinality, right->cardinality);
	}
	// cost is expected_cardinality plus the cost of the previous plans
	size_t cost = expected_cardinality + left->cost + right->cost;
	return make_unique<JoinNode>(set, info, left, right, expected_cardinality, cost);
}

JoinNode *JoinOrderOptimizer::EmitPair(RelationSet *left, RelationSet *right, NeighborInfo *info) {
	// get the left and right join plans
	auto &left_plan = plans[left];
	auto &right_plan = plans[right];
	auto new_set = set_manager.Union(left, right);
	// create the join tree based on combining the two plans
	auto new_plan = CreateJoinTree(new_set, info, left_plan.get(), right_plan.get());
	// check if this plan is the optimal plan we found for this set of relations
	auto entry = plans.find(new_set);
	if (entry == plans.end() || new_plan->cost < entry->second->cost) {
		// the plan is the optimal plan, move it into the dynamic programming tree
		auto result = new_plan.get();
		plans[new_set] = move(new_plan);
		return result;
	}
	return entry->second.get();
}

bool JoinOrderOptimizer::TryEmitPair(RelationSet *left, RelationSet *right, NeighborInfo *info) {
	pairs++;
	if (pairs >= 10000) {
		// when the amount of pairs gets too large we exit the dynamic programming and resort to a greedy algorithm
		// FIXME: simple heuristic currently
		// at 10K pairs stop searching exactly and switch to heuristic
		return false;
	}
	EmitPair(left, right, info);
	return true;
}

bool JoinOrderOptimizer::EmitCSG(RelationSet *node) {
	// create the exclusion set as everything inside the subgraph AND anything with members BELOW it
	unordered_set<size_t> exclusion_set;
	for (size_t i = 0; i < node->relations[0]; i++) {
		exclusion_set.insert(i);
	}
	UpdateExclusionSet(node, exclusion_set);
	// find the neighbors given this exclusion set
	auto neighbors = query_graph.GetNeighbors(node, exclusion_set);
	if (neighbors.size() == 0) {
		return true;
	}
	// we iterate over the neighbors ordered by their first node
	sort(neighbors.begin(), neighbors.end());
	for (auto neighbor : neighbors) {
		// since the GetNeighbors only returns the smallest element in a list, the entry might not be connected to
		// (only!) this neighbor,  hence we have to do a connectedness check before we can emit it
		auto neighbor_relation = set_manager.GetRelation(neighbor);
		auto connection = query_graph.GetConnection(node, neighbor_relation);
		if (connection) {
			if (!TryEmitPair(node, neighbor_relation, connection)) {
				return false;
			}
		}
		if (!EnumerateCmpRecursive(node, neighbor_relation, exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool JoinOrderOptimizer::EnumerateCmpRecursive(RelationSet *left, RelationSet *right,
                                               unordered_set<size_t> exclusion_set) {
	// get the neighbors of the second relation under the exclusion set
	auto neighbors = query_graph.GetNeighbors(right, exclusion_set);
	if (neighbors.size() == 0) {
		return true;
	}
	vector<RelationSet *> union_sets;
	union_sets.resize(neighbors.size());
	for (size_t i = 0; i < neighbors.size(); i++) {
		auto neighbor = set_manager.GetRelation(neighbors[i]);
		// emit the combinations of this node and its neighbors
		auto combined_set = set_manager.Union(right, neighbor);
		if (plans.find(combined_set) != plans.end()) {
			auto connection = query_graph.GetConnection(left, combined_set);
			if (connection) {
				if (!TryEmitPair(left, combined_set, connection)) {
					return false;
				}
			}
		}
		union_sets[i] = combined_set;
	}
	// recursively enumerate the sets
	for (size_t i = 0; i < neighbors.size(); i++) {
		// updated the set of excluded entries with this neighbor
		unordered_set<size_t> new_exclusion_set = exclusion_set;
		new_exclusion_set.insert(neighbors[i]);
		if (!EnumerateCmpRecursive(left, union_sets[i], new_exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool JoinOrderOptimizer::EnumerateCSGRecursive(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	// find neighbors of S under the exlusion set
	auto neighbors = query_graph.GetNeighbors(node, exclusion_set);
	if (neighbors.size() == 0) {
		return true;
	}
	// now first emit the connected subgraphs of the neighbors
	vector<RelationSet *> union_sets;
	union_sets.resize(neighbors.size());
	for (size_t i = 0; i < neighbors.size(); i++) {
		auto neighbor = set_manager.GetRelation(neighbors[i]);
		// emit the combinations of this node and its neighbors
		auto new_set = set_manager.Union(node, neighbor);
		if (plans.find(new_set) != plans.end()) {
			if (!EmitCSG(new_set)) {
				return false;
			}
		}
		union_sets[i] = new_set;
	}
	// recursively enumerate the sets
	for (size_t i = 0; i < neighbors.size(); i++) {
		// updated the set of excluded entries with this neighbor
		unordered_set<size_t> new_exclusion_set = exclusion_set;
		new_exclusion_set.insert(neighbors[i]);
		if (!EnumerateCSGRecursive(union_sets[i], new_exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool JoinOrderOptimizer::SolveJoinOrderExactly() {
	// now we perform the actual dynamic programming to compute the final result
	// we enumerate over all the possible pairs in the neighborhood
	for (size_t i = relations.size(); i > 0; i--) {
		// for every node in the set, we consider it as the start node once
		auto start_node = set_manager.GetRelation(i - 1);
		// emit the start node
		if (!EmitCSG(start_node)) {
			return false;
		}
		// initialize the set of exclusion_set as all the nodes with a number below this
		unordered_set<size_t> exclusion_set;
		for (size_t j = 0; j < i - 1; j++) {
			exclusion_set.insert(j);
		}
		// then we recursively search for neighbors that do not belong to the banned entries
		if (!EnumerateCSGRecursive(start_node, exclusion_set)) {
			return false;
		}
	}
	return true;
}

void JoinOrderOptimizer::SolveJoinOrderApproximately() {
	// at this point, we exited the dynamic programming but did not compute the final join order because it took too
	// long instead, we use a greedy heuristic to obtain a join ordering now we use Greedy Operator Ordering to
	// construct the result tree first we start out with all the base relations (the to-be-joined relations)
	vector<RelationSet *> T;
	for (size_t i = 0; i < relations.size(); i++) {
		T.push_back(set_manager.GetRelation(i));
	}
	while (T.size() > 1) {
		// now in every step of the algorithm, we greedily pick the join between the to-be-joined relations that has the
		// smallest cost. This is O(r^2) per step, and every step will reduce the total amount of relations to-be-joined
		// by 1, so the total cost is O(r^3) in the amount of relations
		size_t best_left = 0, best_right = 0;
		JoinNode *best_connection = nullptr;
		for (size_t i = 0; i < T.size(); i++) {
			auto left = T[i];
			for (size_t j = i + 1; j < T.size(); j++) {
				auto right = T[j];
				// check if we can connect these two relations
				auto connection = query_graph.GetConnection(left, right);
				if (connection) {
					// we can! check the cost of this connection
					auto node = EmitPair(left, right, connection);
					if (!best_connection || node->cost < best_connection->cost) {
						// best pair found so far
						best_connection = node;
						best_left = i;
						best_right = j;
					}
				}
			}
		}
		if (!best_connection) {
			// could not find a connection, but we were not done with finding a completed plan
			// we have to add a cross product; we add it between the two smallest relations
			JoinNode *smallest_plans[2] = {nullptr};
			size_t smallest_index[2];
			for (size_t i = 0; i < T.size(); i++) {
				// get the plan for this relation
				auto current_plan = plans[T[i]].get();
				// check if the cardinality is smaller than the smallest two found so far
				for (size_t j = 0; j < 2; j++) {
					if (!smallest_plans[j] || smallest_plans[j]->cardinality > current_plan->cardinality) {
						smallest_plans[j] = current_plan;
						smallest_index[j] = i;
						break;
					}
				}
			}
			assert(smallest_plans[0] && smallest_plans[1]);
			assert(smallest_index[0] != smallest_index[1]);
			auto left = smallest_plans[0]->set, right = smallest_plans[1]->set;
			// create a cross product edge (i.e. edge with empty filter) between these two sets in the query graph
			query_graph.CreateEdge(left, right, nullptr);
			// now emit the pair and continue with the algorithm
			auto connection = query_graph.GetConnection(left, right);
			assert(connection);

			best_connection = EmitPair(left, right, connection);
			best_left = smallest_index[0];
			best_right = smallest_index[1];
			// the code below assumes best_right > best_left
			if (best_left > best_right) {
				swap(best_left, best_right);
			}
		}
		// now update the to-be-checked pairs
		// remove left and right, and add the combination

		// important to erase the biggest element first
		// if we erase the smallest element first the index of the biggest element changes
		assert(best_right > best_left);
		T.erase(T.begin() + best_right);
		T.erase(T.begin() + best_left);
		T.push_back(best_connection->set);
	}
}

void JoinOrderOptimizer::SolveJoinOrder() {
	// first try to solve the join order exactly
	if (!SolveJoinOrderExactly()) {
		// otherwise, if that times out we resort to a greedy algorithm
		SolveJoinOrderApproximately();
	}
}

void JoinOrderOptimizer::GenerateCrossProducts() {
	// generate a set of cross products to combine the currently available plans into a full join plan
	// we create edges between every relation with a high cost
	for (size_t i = 0; i < relations.size(); i++) {
		auto left = set_manager.GetRelation(i);
		for (size_t j = 0; j < relations.size(); j++) {
			if (i != j) {
				auto right = set_manager.GetRelation(j);
				query_graph.CreateEdge(left, right, nullptr);
				query_graph.CreateEdge(right, left, nullptr);
			}
		}
	}
}

static unique_ptr<LogicalOperator> ExtractRelation(Relation &rel) {
	auto &children = rel.parent->children;
	for (size_t i = 0; i < children.size(); i++) {
		if (children[i].get() == rel.op) {
			// found it! take ownership of it from the parent
			auto result = move(children[i]);
			children.erase(children.begin() + i);
			return result;
		}
	}
	throw Exception("Could not find relation in parent node (?)");
}

pair<RelationSet *, unique_ptr<LogicalOperator>>
JoinOrderOptimizer::GenerateJoins(vector<unique_ptr<LogicalOperator>> &extracted_relations, JoinNode *node) {
	RelationSet *result_relation;
	unique_ptr<LogicalOperator> result_operator;
	if (node->left && node->right) {
		// generate the left and right children
		auto left = GenerateJoins(extracted_relations, node->left);
		auto right = GenerateJoins(extracted_relations, node->right);

		if (node->info->filters.size() == 0) {
			// no filters, create a cross product
			auto join = make_unique<LogicalCrossProduct>();
			join->children.push_back(move(left.second));
			join->children.push_back(move(right.second));
			result_operator = move(join);
		} else {
			// we have filters, create a join node
			auto join = make_unique<LogicalJoin>(JoinType::INNER);
			join->children.push_back(move(left.second));
			join->children.push_back(move(right.second));
			// set the join conditions from the join node
			for (auto &f : node->info->filters) {
				// extract the filter from the operator it originally belonged to
				assert(filters[f->filter_index]);
				auto condition = move(filters[f->filter_index]);
				// now create the actual join condition
				assert((RelationSet::IsSubset(left.first, f->left_set) &&
				        RelationSet::IsSubset(right.first, f->right_set)) ||
				       (RelationSet::IsSubset(left.first, f->right_set) &&
				        RelationSet::IsSubset(right.first, f->left_set)));
				JoinCondition cond;
				assert(condition->GetExpressionClass() == ExpressionClass::COMPARISON);
				auto &comparison = (ComparisonExpression &)*condition;
				// we need to figure out which side is which by looking at the relations available to us
				bool invert = RelationSet::IsSubset(left.first, f->left_set) ? false : true;
				cond.left = !invert ? move(comparison.left) : move(comparison.right);
				cond.right = !invert ? move(comparison.right) : move(comparison.left);
				cond.comparison = condition->type;
				if (invert) {
					// reverse comparison expression if we reverse the order of the children
					cond.comparison = ComparisonExpression::FlipComparisionExpression(cond.comparison);
				}
				join->conditions.push_back(move(cond));
			}
			assert(join->conditions.size() > 0);
			result_operator = move(join);
		}
		result_relation = set_manager.Union(left.first, right.first);
	} else {
		// base node, get the entry from the list of extracted relations
		assert(node->set->count == 1);
		assert(extracted_relations[node->set->relations[0]]);
		result_relation = node->set;
		result_operator = move(extracted_relations[node->set->relations[0]]);
	}
	// check if we should do a pushdown on this node
	// basically, any remaining filter that is a subset of the current relation will no longer be used in joins
	// hence we should push it here
	for (size_t i = 0; i < filter_infos.size(); i++) {
		// check if the filter has already been extracted
		auto info = filter_infos[i].get();
		if (filters[info->filter_index]) {
			// now check if the filter is a subset of the current relation
			// note that infos with an empty relation set are a special case and we do not push them down
			if (info->set->count > 0 && RelationSet::IsSubset(result_relation, info->set)) {
				auto filter = move(filters[info->filter_index]);
				// if it is, we can push the filter
				// there are two cases here
				// (1) the filter is a ComparisonExpression, in which case we can push it into a join (if it exists)
				// (2) the filter is anything else, in which case we push it into a filter
				// first check the class
				if (filter->GetExpressionClass() == ExpressionClass::COMPARISON) {
					// comparison, check if there is a join
					if (result_operator->type == LogicalOperatorType::JOIN) {
						// join, push it into the expression list
						result_operator->expressions.push_back(move(filter));
					} else if (result_operator->type == LogicalOperatorType::FILTER) {
						// filter, check if the underlying type is a join
						if (result_operator->children[0]->type == LogicalOperatorType::JOIN) {
							// join, push it there
							result_operator->children[0]->expressions.push_back(move(filter));
						} else {
							// not a join, push it to the filter
							result_operator->expressions.push_back(move(filter));
						}
					} else {
						// not a filter or a join, push a filter
						result_operator = PushFilter(move(result_operator), move(filter));
					}
				} else {
					// not a comparison, just push it into a filter
					result_operator = PushFilter(move(result_operator), move(filter));
				}
			}
		}
	}
	return make_pair(result_relation, move(result_operator));
}

unique_ptr<LogicalOperator> JoinOrderOptimizer::RewritePlan(unique_ptr<LogicalOperator> plan, JoinNode *node) {
	// now we have to rewrite the plan
	bool root_is_join = plan->children.size() > 1;

	// first we will extract all relations from the main plan
	vector<unique_ptr<LogicalOperator>> extracted_relations;
	for (size_t i = 0; i < relations.size(); i++) {
		extracted_relations.push_back(ExtractRelation(*relations[i]));
	}
	// now we generate the actual joins
	auto join_tree = GenerateJoins(extracted_relations, node);
	// perform the final pushdown of remaining filters
	for (size_t i = 0; i < filters.size(); i++) {
		// check if the filter has already been extracted
		if (filters[i]) {
			// if not we need to push it
			join_tree.second = PushFilter(move(join_tree.second), move(filters[i]));
		}
	}

	// find the first join in the relation to know where to place this node
	if (root_is_join) {
		// first node is the join, return it immediately
		return move(join_tree.second);
	}
	assert(plan->children.size() == 1);
	// have to move up through the relations
	auto op = plan.get();
	auto parent = plan.get();
	while (op->type != LogicalOperatorType::CROSS_PRODUCT && op->type != LogicalOperatorType::JOIN) {
		assert(op->children.size() == 1);
		parent = op;
		op = op->children[0].get();
	}
	// have to replace at this node
	parent->children[0] = move(join_tree.second);
	return ResolveJoinConditions(move(plan));
}

// the join ordering is pretty much a straight implementation of the paper "Dynamic Programming Strikes Back" by Guido
// Moerkotte and Thomas Neumannn, see that paper for additional info/documentation bonus slides:
// https://db.in.tum.de/teaching/ws1415/queryopt/chapter3.pdf?lang=de
// FIXME: incorporate cardinality estimation into the plans, possibly by pushing samples?
unique_ptr<LogicalOperator> JoinOrderOptimizer::Optimize(unique_ptr<LogicalOperator> plan) {
	assert(filters.size() == 0 && relations.size() == 0); // assert that the JoinOrderOptimizer has not been used before
	LogicalOperator *op = plan.get();
	// now we optimize the current plan
	// we skip past until we find the first projection, we do this because the HAVING clause inserts a Filter AFTER the
	// group by and this filter cannot be reordered
	// extract a list of all relations that have to be joined together
	// and a list of all conditions that is applied to them
	vector<LogicalOperator *> filter_operators;
	if (!ExtractJoinRelations(*op, filter_operators)) {
		// do not support reordering this type of plan
		return ResolveJoinConditions(move(plan));
	}
	if (relations.size() <= 1) {
		// at most one relation, nothing to reorder
		return ResolveJoinConditions(move(plan));
	}
	// now that we know we are going to perform join ordering we actually extract the filters
	for (auto &op : filter_operators) {
		ExtractFilters(op, filters);
	}
	// create potential edges from the comparisons
	for (size_t i = 0; i < filters.size(); i++) {
		auto &filter = filters[i];
		auto info = make_unique<FilterInfo>();
		auto filter_info = info.get();
		filter_infos.push_back(move(info));
		// first extract the relation set for the entire filter
		unordered_set<size_t> bindings;
		ExtractBindings(*filter, bindings);
		filter_info->set = set_manager.GetRelation(bindings);
		filter_info->filter_index = i;
		// now check if it can be used as a join predicate
		if (filter->GetExpressionClass() == ExpressionClass::COMPARISON) {
			auto comparison = (ComparisonExpression *)filter.get();
			// extract the bindings that are required for the left and right side of the comparison
			unordered_set<size_t> left_bindings, right_bindings;
			ExtractBindings(*comparison->left, left_bindings);
			ExtractBindings(*comparison->right, right_bindings);
			if (left_bindings.size() > 0 && right_bindings.size() > 0) {
				// both the left and the right side have bindings
				// first create the relation sets, if they do not exist
				filter_info->left_set = set_manager.GetRelation(left_bindings);
				filter_info->right_set = set_manager.GetRelation(right_bindings);
				// we can only create a meaningful edge if the sets are not exactly the same
				if (filter_info->left_set != filter_info->right_set) {
					// check if the sets are disjoint
					if (Disjoint(left_bindings, right_bindings)) {
						// they are disjoint, we only need to create one set of edges in the join graph
						query_graph.CreateEdge(filter_info->left_set, filter_info->right_set, filter_info);
						query_graph.CreateEdge(filter_info->right_set, filter_info->left_set, filter_info);
					} else {
						// the sets are not disjoint, we create two sets of edges
						auto left_difference = set_manager.Difference(filter_info->left_set, filter_info->right_set);
						auto right_difference = set_manager.Difference(filter_info->right_set, filter_info->left_set);
						// -> LEFT <-> RIGHT \ LEFT
						query_graph.CreateEdge(filter_info->left_set, right_difference, filter_info);
						query_graph.CreateEdge(right_difference, filter_info->left_set, filter_info);
						// -> RIGHT <-> LEFT \ RIGHT
						query_graph.CreateEdge(left_difference, filter_info->right_set, filter_info);
						query_graph.CreateEdge(filter_info->right_set, left_difference, filter_info);
					}
					continue;
				}
			}
		}
	}
	// now use dynamic programming to figure out the optimal join order
	// First we initialize each of the single-node plans with themselves and with their cardinalities these are the leaf
	// nodes of the join tree NOTE: we can just use pointers to RelationSet* here because the GetRelation function
	// ensures that a unique combination of relations will have a unique RelationSet object.
	for (size_t i = 0; i < relations.size(); i++) {
		auto &rel = *relations[i];
		auto node = set_manager.GetRelation(i);
		plans[node] = make_unique<JoinNode>(node, rel.op->EstimateCardinality());
	}
	// now we perform the actual dynamic programming to compute the final result
	SolveJoinOrder();
	// now the optimal join path should have been found
	// get it from the node
	unordered_set<size_t> bindings;
	for (size_t i = 0; i < relations.size(); i++) {
		bindings.insert(i);
	}
	auto total_relation = set_manager.GetRelation(bindings);
	auto final_plan = plans.find(total_relation);
	if (final_plan == plans.end()) {
		// could not find the final plan
		// this should only happen in case the sets are actually disjunct
		// in this case we need to generate cross product to connect the disjoint sets
		GenerateCrossProducts();
		//! solve the join order again
		SolveJoinOrder();
		// now we can obtain the final plan!
		final_plan = plans.find(total_relation);
		assert(final_plan != plans.end());
	}
	// now perform the actual reordering
	return RewritePlan(move(plan), final_plan->second.get());
}