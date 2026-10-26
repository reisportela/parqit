
// parqit: retain volatile window keys without evaluating them independently
// for hashing, sorting, peers or RANGE boundaries. This runs before optimisation.
static unique_ptr<Expression> &ParqitUncastKey(unique_ptr<Expression> &expr) {
	if (BoundCastExpression::IsCast(*expr)) {
		return ParqitUncastKey(BoundCastExpression::ChildMutable(expr->Cast<BoundFunctionExpression>()));
	}
	return expr;
}

static void ParqitReplaceRangeKey(unique_ptr<Expression> &expr, WindowBoundary boundary,
                                  const Expression &key, const Expression &reference) {
	if (boundary != WindowBoundary::EXPR_PRECEDING_RANGE && boundary != WindowBoundary::EXPR_FOLLOWING_RANGE) {
		return;
	}
	// The binder constructs +/- (copied order key, offset), possibly with casts.
	// Only replace the first operand: an identical volatile offset is another call.
	auto &operation = ParqitUncastKey(expr);
	if (operation->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InternalException("Unexpected bound RANGE expression for volatile window key");
	}
	auto &children = operation->Cast<BoundFunctionExpression>().GetChildrenMutable();
	if (children.size() != 2) {
		throw InternalException("Unexpected bound RANGE operands for volatile window key");
	}
	auto &copied_key = ParqitUncastKey(children[0]);
	if (!copied_key->Equals(key)) {
		throw InternalException("RANGE boundary lost its volatile order key before materialisation");
	}
	copied_key = reference.Copy();
}

static bool ParqitSameWindowPartitions(const BoundWindowExpression &left, const BoundWindowExpression &right) {
	// Unlike set comparison, this preserves separate volatile occurrences.
	if (left.Partitions().size() != right.Partitions().size()) return false;
	for (idx_t i = 0; i < left.Partitions().size(); ++i) {
		if (!left.Partitions()[i]->Equals(*right.Partitions()[i])) return false;
	}
	return true;
}

static void ParqitMaterialiseWindowKeys(Binder &binder, LogicalOperator &root, LogicalOperator &op) {
	auto &window = op.Cast<LogicalWindow>();
	bool needed = false;
	for (auto &expr : window.expressions) {
		auto &w = expr->Cast<BoundWindowExpression>();
		for (auto &key : w.Partitions()) needed = needed || key->IsVolatile();
		for (auto &order : w.OrderBy()) needed = needed || order.expression->IsVolatile();
	}
	if (!needed) return;

	// Group identical OVER definitions before changing any expression. Common
	// ORDER prefixes share their keys, as in DuckDB's physical window planner.
	vector<vector<idx_t>> groups;
	vector<idx_t> representatives, remaining;
	for (idx_t i = 0; i < window.expressions.size(); ++i) remaining.push_back(i);
	while (!remaining.empty()) {
		auto representative = remaining[0];
		vector<idx_t> matching, next;
		for (auto i : remaining) {
			auto &w = window.expressions[i]->Cast<BoundWindowExpression>();
			auto &over = window.expressions[representative]->Cast<BoundWindowExpression>();
			if (!ParqitSameWindowPartitions(over, w) ||
			    over.GetSharedOrders(w) != MinValue<idx_t>(over.OrderBy().size(), w.OrderBy().size())) {
				next.push_back(i);
				continue;
			}
			matching.push_back(i);
			if (w.OrderBy().size() > over.OrderBy().size()) representative = i;
		}
		groups.push_back(std::move(matching));
		representatives.push_back(representative);
		remaining = std::move(next);
	}

	auto &input = window.children[0];
	input->ResolveOperatorTypes();
	const auto bindings = input->GetColumnBindings();
	const auto index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	vector<ColumnBinding> replacement_bindings;
	for (idx_t i = 0; i < bindings.size(); ++i) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(input->types[i], bindings[i]));
		replacement_bindings.emplace_back(index, ProjectionIndex(i));
	}
	auto materialise = [&](const Expression &key) {
		auto reference = make_uniq<BoundColumnRefExpression>(
		    key.GetReturnType(), ColumnBinding(index, ProjectionIndex(expressions.size())));
		expressions.push_back(key.Copy());
		return reference;
	};
	for (idx_t group_idx = 0; group_idx < groups.size(); ++group_idx) {
		auto &over = window.expressions[representatives[group_idx]]->Cast<BoundWindowExpression>();
		for (idx_t p = 0; p < over.Partitions().size(); ++p) {
			if (!over.Partitions()[p]->IsVolatile()) continue;
			auto reference = materialise(*over.Partitions()[p]);
			for (auto i : groups[group_idx]) {
				window.expressions[i]->Cast<BoundWindowExpression>().PartitionsMutable()[p] = reference->Copy();
			}
		}
		for (idx_t o = 0; o < over.OrderBy().size(); ++o) {
			if (!over.OrderBy()[o].expression->IsVolatile()) continue;
			// Keep casts around the same exact key in the order and frame operands.
			auto key = ParqitUncastKey(over.OrderByMutable()[o].expression)->Copy();
			auto reference = materialise(*key);
			for (auto i : groups[group_idx]) {
				auto &w = window.expressions[i]->Cast<BoundWindowExpression>();
				if (o >= w.OrderBy().size()) continue;
				if (o == 0) {
					ParqitReplaceRangeKey(w.StartExprMutable(), w.WindowStart(), *key, *reference);
					ParqitReplaceRangeKey(w.EndExprMutable(), w.WindowEnd(), *key, *reference);
				}
				ParqitUncastKey(w.OrderByMutable()[o].expression) = reference->Copy();
			}
		}
	}
	auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
	CorrelatedColumnBindingReplacer replacer;
	replacer.AddReplacements(bindings, replacement_bindings);
	replacer.stop_operator = *projection;
	projection->AddChild(std::move(input));
	input = std::move(projection);
	replacer.VisitOperator(root);
}

