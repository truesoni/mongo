/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/stage_builder/sbe/builder.h"

#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/gen_expression.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"

namespace mongo::stage_builder {
namespace {
template <typename F>
struct FieldPathAndCondPreVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    explicit FieldPathAndCondPreVisitor(const F& fn) : _fn(fn) {}

    void visit(const ExpressionFieldPath* expr) final {
        _fn(expr);
    }

    F _fn;
};

/**
 * Walks through the 'expr' expression tree and whenever finds an 'ExpressionFieldPath', calls
 * the 'fn' function. Type requirement for 'fn' is it must have a const 'ExpressionFieldPath'
 * pointer parameter.
 */
template <typename F>
void walkAndActOnFieldPaths(Expression* expr, const F& fn) {
    FieldPathAndCondPreVisitor<F> preVisitor(fn);
    ExpressionWalker walker(&preVisitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
    expression_walker::walk(expr, &walker);
}

/**
 * Compute what values 'groupNode' will need from its child node in order to build expressions for
 * the group-by key ("_id") and the accumulators.
 */
MONGO_COMPILER_NOINLINE
PlanStageReqs computeChildReqsForGroup(const PlanStageReqs& reqs, const GroupNode& groupNode) {
    constexpr bool allowCallGenCheapSortKey = true;

    auto childReqs = reqs.copyForChild().setResultObj().clearAllFields();

    // If the group node references any top level fields, we take all of them and add them to
    // 'childReqs'. Note that this happens regardless of whether we need the whole document because
    // it can be the case that this stage references '$$ROOT' as well as some top level fields.
    if (auto topLevelFields = getTopLevelFields(groupNode.requiredFields);
        !topLevelFields.empty()) {
        childReqs.setFields(std::move(topLevelFields));
    }

    if (!groupNode.needWholeDocument) {
        // Tracks if any sort keys we need to generate depend on the having a materialized
        // result object.

        // Some accumulators (like $top and $bottom) need to generate sort keys. Here we loop
        // over 'groupNode.accumulators' to observe what each accumulator's needs are.
        bool sortKeysNeedRootDoc = false;
        for (const auto& accStmt : groupNode.accumulators) {
            if (auto sortPattern = getSortPattern(accStmt)) {
                auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);

                if (!plan.fieldsForSortKeys.empty()) {
                    // If this accumulator needs specific top-level fields in slots, add the
                    // appropriate kField reqs to 'childReqs'.
                    childReqs.setFields(std::move(plan.fieldsForSortKeys));
                }

                if (plan.needsResultObj) {
                    // If this accumulator needs the whole result object, set 'sortKeysNeedRootDoc'
                    // to true.
                    sortKeysNeedRootDoc = true;
                }
            }
        }

        // If no accumulator requires the whole result object for generating sort keys, then we
        // can clear the result requirement from 'childReqs'.
        if (!sortKeysNeedRootDoc) {
            childReqs.clearResult();
        }
    }

    return childReqs;
}

/**
 * Collect the FieldPath expressions referenced by a GroupNode that should be exposed in a slot for
 * the group stage to work properly.
 */
MONGO_COMPILER_NOINLINE
StringMap<const ExpressionFieldPath*> collectFieldPaths(const GroupNode* groupNode) {
    StringMap<const ExpressionFieldPath*> groupFieldMap;
    auto accumulateFieldPaths = [&](const ExpressionFieldPath* fieldExpr) {
        // We optimize neither a field path for the top-level document itself nor a field path
        // that refers to a variable instead.
        if (fieldExpr->getFieldPath().getPathLength() == 1 || fieldExpr->isVariableReference()) {
            return;
        }

        // Don't generate an expression if we have one already.
        std::string fp = fieldExpr->getFieldPathWithoutCurrentPrefix().fullPath();
        if (groupFieldMap.count(fp)) {
            return;
        }
        // Neither if it's a top level field which already have a slot.
        if (fieldExpr->getFieldPath().getPathLength() != 2) {
            groupFieldMap.emplace(fp, fieldExpr);
        }
    };
    // Walk over all field paths involved in this $group stage.
    walkAndActOnFieldPaths(groupNode->groupByExpression.get(), accumulateFieldPaths);
    for (const auto& accStmt : groupNode->accumulators) {
        walkAndActOnFieldPaths(accStmt.expr.argument.get(), accumulateFieldPaths);
    }
    return groupFieldMap;
}

namespace {
struct PartitionedFieldPathExprs {
    StringDataMap<const ExpressionFieldPath*> exprsOnBlockSlots;
    StringDataMap<const ExpressionFieldPath*> exprsOnScalarSlots;
};

/*
 * Returns whether or not the given field path expression references a block. Assumes
 * that we are in block mode.
 */
bool doesExpressionReferenceBlock(const PlanStageSlots& outputs,
                                  const ExpressionFieldPath& expressionFieldPath) {
    tassert(8829002, "Expected outputs to have block output", outputs.hasBlockOutput());

    if (expressionFieldPath.getVariableId() != Variables::kRootId) {
        return false;
    }

    auto& fieldPath = expressionFieldPath.getFieldPath();

    // The first component should be $$CURRENT.
    tassert(
        8829001, "Field path should have more than one component", fieldPath.getPathLength() > 1);

    // Top level field is at index 1.
    auto firstComponent = fieldPath.getFieldName(1);

    // Since we're in block mode, the child MUST provide this kField, as there is no result
    // obj.  Note: in the future, it may be possible that the child provides the full
    // kPathExpr, but not the kField for the top level. This code will have to handle that
    // case.

    auto outputSlot =
        outputs.get(PlanStageReqs::UnownedSlotName(PlanStageReqs::kField, firstComponent));

    // Skip any field path expressions on blocks. Those will be computed after block_to_row.
    if (auto typeSig = outputSlot.getTypeSignature()) {
        if (typeSig->containsAny(TypeSignature::kBlockType.include(TypeSignature::kCellType))) {
            return true;
        }
    }

    return false;
}

/**
 * Splits the given map of field path expressions into those which refer to block fields and those
 * which refer to scalar fields.
 */
MONGO_COMPILER_NOINLINE
PartitionedFieldPathExprs partitionFieldPathExprsByBlock(
    PlanStageSlots& outputs, const StringMap<const ExpressionFieldPath*>& groupFieldMapIn) {

    PartitionedFieldPathExprs out;
    for (auto& [fieldStr, expressionFieldPath] : groupFieldMapIn) {
        if (doesExpressionReferenceBlock(outputs, *expressionFieldPath)) {
            out.exprsOnBlockSlots.emplace(fieldStr, expressionFieldPath);
        } else {
            out.exprsOnScalarSlots.emplace(fieldStr, expressionFieldPath);
        }
    }
    return out;
}
}  // namespace

/**
 * Given a list of field path expressions used in the group-by ('_id') and accumulator expressions
 * of a $group, populate a slot in 'outputs' for each path found. Each slot is bound to an SBE
 * EExpression (via a ProjectStage) that evaluates the path traversal.
 */
MONGO_COMPILER_NOINLINE
SbStage projectFieldPathsToPathExprSlots(
    StageBuilderState& state,
    const GroupNode& groupNode,
    SbStage stage,
    PlanStageSlots& outputs,
    const StringDataMap<const ExpressionFieldPath*>& groupFieldMap) {
    SbBuilder b(state, groupNode.nodeId());

    SbExprOptSbSlotVector projects;
    for (auto& fp : groupFieldMap) {
        projects.emplace_back(stage_builder::generateExpression(
                                  state, fp.second, outputs.getResultObjIfExists(), outputs),
                              boost::none);
    }

    if (!projects.empty()) {
        auto [outStage, outSlots] =
            b.makeProject(buildVariableTypes(outputs), std::move(stage), std::move(projects));
        stage = std::move(outStage);

        size_t i = 0;
        for (auto& fp : groupFieldMap) {
            auto name = PlanStageSlots::OwnedSlotName(PlanStageSlots::kPathExpr, fp.first);
            outputs.set(std::move(name), outSlots[i]);
            ++i;
        }
    }

    return stage;
}
/**
 * Ensure that all kPathExpr reqs are available in slots.
 */
MONGO_COMPILER_NOINLINE
SbStage makePathExprsAvailableInSlots(
    StageBuilderState& state,
    const GroupNode& groupNode,
    SbStage stage,
    PlanStageSlots& outputs,
    const StringMap<const ExpressionFieldPath*>& groupFieldMapIn) {

    if (groupFieldMapIn.empty()) {
        // No work to do.
        return stage;
    }

    SbBuilder b(state, groupNode.nodeId());

    if (outputs.hasBlockOutput()) {
        // We are currently running in block mode. Some slots will contain blocks, and others may
        // contain scalars. The scalar slots contain values common to the entire block, like the
        // timeseries 'meta' field.

        // First we compute the field path expressions for any expressions which are on scalars. We
        // want to do these before we close the block processing pipeline.

        auto [blockFieldPathExprs, nonBlockFieldPathExprs] =
            partitionFieldPathExprsByBlock(outputs, groupFieldMapIn);

        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, nonBlockFieldPathExprs);

        if (blockFieldPathExprs.empty()) {
            // If there are no block field path exprs, we actually don't need to close the block
            // pipeline.
            return stage;
        }

        stage = buildBlockToRow(std::move(stage), state, outputs);


        // Now that we've done the block to row, evaluate the path
        // expressions for the slots that were blocks, and are now scalars.
        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, blockFieldPathExprs);
    } else {
        // We have to convert to StringDataMap to call projectFieldPathsToPathExprSlots().
        StringDataMap<const ExpressionFieldPath*> groupFieldMap;
        groupFieldMap.insert(groupFieldMapIn.begin(), groupFieldMapIn.end());
        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, groupFieldMap);
    }
    return stage;
}

MONGO_COMPILER_NOINLINE
SbExpr::Vector generateGroupByKeyExprs(StageBuilderState& state,
                                       Expression* idExpr,
                                       const PlanStageSlots& outputs) {
    SbExprBuilder b(state);
    SbExpr::Vector exprs;
    auto rootSlot = outputs.getResultObjIfExists();

    auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr);
    if (idExprObj) {
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            exprs.emplace_back(generateExpression(state, fieldExpr.get(), rootSlot, outputs));
        }
        // When there's only one field in the document _id expression, 'Nothing' is converted to
        // 'Null'.
        // TODO SERVER-21992: Remove the following block because this block emulates the classic
        // engine's buggy behavior. With index that can handle 'Nothing' and 'Null' differently,
        // SERVER-21992 issue goes away and the distinct scan should be able to return 'Nothing'
        // and 'Null' separately.
        if (exprs.size() == 1) {
            exprs[0] = b.makeFillEmptyNull(std::move(exprs[0]));
        }
    } else {
        // The group-by field may end up being 'Nothing' and in that case _id: null will be
        // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
        exprs.emplace_back(
            b.makeFillEmptyNull(generateExpression(state, idExpr, rootSlot, outputs)));
    }

    return exprs;
}

using TagAndValue = std::pair<sbe::value::TypeTags, sbe::value::Value>;

std::variant<Expression*, TagAndValue> getTopBottomNValueExprHelper(
    const AccumulationStatement& accStmt) {
    auto accOp = AccumOp{accStmt};

    auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get());
    auto expConst =
        !expObj ? dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get()) : nullptr;

    tassert(5807015,
            str::stream() << accOp.getOpName() << " accumulator must have an object argument",
            expObj || (expConst && expConst->getValue().isObject()));

    if (expObj) {
        for (auto& [key, value] : expObj->getChildExpressions()) {
            if (key == AccumulatorN::kFieldNameOutput) {
                return value.get();
            }
        }
    } else {
        auto objConst = expConst->getValue();
        auto objBson = objConst.getDocument().toBson();
        // `outputField` may reference data in objBson, so must not outlive it.
        auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
        if (outputField.ok()) {
            return sbe::bson::convertFrom<false /* not a view */>(outputField);
        }
    }

    tasserted(5807016,
              str::stream() << accOp.getOpName()
                            << " accumulator must have an output field in the argument");
}

SbExpr getTopBottomNValueExpr(StageBuilderState& state,
                              const AccumulationStatement& accStmt,
                              const PlanStageSlots& outputs) {
    SbExprBuilder b(state);

    auto valueExpr = getTopBottomNValueExprHelper(accStmt);

    if (holds_alternative<Expression*>(valueExpr)) {
        auto rootSlot = outputs.getResultObjIfExists();
        auto* expr = get<Expression*>(valueExpr);
        return b.makeFillEmptyNull(generateExpression(state, expr, rootSlot, outputs));
    } else {
        auto [tag, val] = get<TagAndValue>(valueExpr);
        return b.makeConstant(tag, val);
    }
}

std::pair<SbExpr::Vector, bool> getBlockTopBottomNValueExpr(StageBuilderState& state,
                                                            const AccumulationStatement& accStmt,
                                                            const PlanStageSlots& outputs) {
    SbExprBuilder b(state);
    bool isArray = false;

    auto valueExpr = getTopBottomNValueExprHelper(accStmt);

    if (holds_alternative<Expression*>(valueExpr)) {
        auto rootSlot = outputs.getResultObjIfExists();
        auto* expr = get<Expression*>(valueExpr);
        auto* arrayExpr = dynamic_cast<ExpressionArray*>(expr);

        if (arrayExpr) {
            isArray = true;

            // If the output field from the $top/$bottom AccumulationStatement is an
            // ExpressionArray, then we set 'isArray' to true and return a vector of the
            // element expressions.
            SbExpr::Vector sbExprs;
            for (size_t i = 0; i < arrayExpr->getChildren().size(); ++i) {
                auto* elemExpr = arrayExpr->getChildren()[i].get();
                sbExprs.emplace_back(
                    b.makeFillEmptyNull(generateExpression(state, elemExpr, rootSlot, outputs)));
            }

            return {std::move(sbExprs), isArray};
        }

        auto sbExpr = b.makeFillEmptyNull(generateExpression(state, expr, rootSlot, outputs));
        return {SbExpr::makeSeq(std::move(sbExpr)), isArray};
    } else {
        auto [tag, val] = get<TagAndValue>(valueExpr);
        return {SbExpr::makeSeq(b.makeConstant(tag, val)), isArray};
    }
}

SbExpr getTopBottomNSortByExpr(StageBuilderState& state,
                               const AccumulationStatement& accStmt,
                               const PlanStageSlots& outputs,
                               SbExpr sortSpecExpr) {
    constexpr bool allowCallGenCheapSortKey = true;

    SbExprBuilder b(state);

    auto sortPattern = getSortPattern(accStmt);
    tassert(8774900, "Expected sort pattern for $top/$bottom accumulator", sortPattern.has_value());

    auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);
    auto sortKeys = buildSortKeys(state, plan, *sortPattern, outputs, std::move(sortSpecExpr));

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        auto fullKeyExpr = [&] {
            if (sortPattern->size() == 1) {
                // When the sort pattern has only one part, we return the sole part's key expr.
                return std::move(sortKeys.keyExprs[0]);
            } else if (sortPattern->size() > 1) {
                // When the sort pattern has more than one part, we return an array containing
                // each part's key expr (in order).
                return b.makeFunction("newArray", std::move(sortKeys.keyExprs));
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        if (sortKeys.parallelArraysCheckExpr) {
            // If 'parallelArraysCheckExpr' is not null, inject it into 'fullKeyExpr'.
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            fullKeyExpr = b.makeIf(std::move(sortKeys.parallelArraysCheckExpr),
                                   std::move(fullKeyExpr),
                                   std::move(parallelArraysError));
        }

        return fullKeyExpr;
    } else if (plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        // generateCheapSortKey() returns a SortKeyComponentVector, but we need an array of
        // keys (or the sole part's key in cases where the sort pattern has only one part),
        // so we generate a call to sortKeyComponentVectorToArray() to perform the conversion.
        return b.makeFunction("sortKeyComponentVectorToArray", std::move(sortKeys.fullKeyExpr));
    } else {
        MONGO_UNREACHABLE;
    }
}

std::pair<SbExpr::Vector, bool> getBlockTopBottomNSortByExpr(StageBuilderState& state,
                                                             const AccumulationStatement& accStmt,
                                                             const PlanStageSlots& outputs,
                                                             SbExpr sortSpecExpr) {
    constexpr bool allowCallGenCheapSortKey = true;
    bool useMK = false;

    SbExprBuilder b(state);

    auto sortPattern = getSortPattern(accStmt);
    tassert(8448719, "Expected sort pattern for $top/$bottom accumulator", sortPattern.has_value());

    auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);
    auto sortKeys = buildSortKeys(state, plan, *sortPattern, outputs, std::move(sortSpecExpr));

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        auto keyExprs = [&] {
            if (sortPattern->size() == 1) {
                // When the sort pattern has only one part, we return the sole part's key expr.
                return SbExpr::makeSeq(std::move(sortKeys.keyExprs[0]));
            } else if (sortPattern->size() > 1) {
                // When the sort pattern has more than one part, we return an array containing
                // each part's key expr (in order).
                useMK = true;
                return std::move(sortKeys.keyExprs);
            } else {
                return SbExpr::makeSeq(b.makeFunction("newArray"));
            }
        }();

        if (sortKeys.parallelArraysCheckExpr) {
            // If 'parallelArraysCheckExpr' is not null, inject it into 'fullKeyExpr'.
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            tassert(8448720, "Expected vector to be non-empty", !keyExprs.empty());

            keyExprs[0] = b.makeIf(std::move(sortKeys.parallelArraysCheckExpr),
                                   std::move(keyExprs[0]),
                                   std::move(parallelArraysError));
        }

        return {std::move(keyExprs), useMK};
    } else if (plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        // generateCheapSortKey() returns a SortKeyComponentVector, but we need an array of
        // keys (or the sole part's key in cases where the sort pattern has only one part),
        // so we generate a call to sortKeyComponentVectorToArray() to perform the conversion.
        auto fullKeyExpr =
            b.makeFunction("sortKeyComponentVectorToArray", std::move(sortKeys.fullKeyExpr));

        return {SbExpr::makeSeq(std::move(fullKeyExpr)), useMK};
    } else {
        MONGO_UNREACHABLE;
    }
}

AccumInputsPtr generateAccumExprs(StageBuilderState& state,
                                  const AccumulationStatement& accStmt,
                                  const PlanStageSlots& outputs) {
    auto accOp = AccumOp{accStmt};

    auto rootSlot = outputs.getResultObjIfExists();

    AccumInputsPtr inputs;

    // For $topN and $bottomN, we need to pass multiple SbExprs to buildAddExprs()
    // (an "input" expression and a "sortBy" expression).
    if (isTopBottomN(accStmt)) {
        auto specSlot = SbSlot{state.getSortSpecSlot(&accStmt)};

        inputs = std::make_unique<AddTopBottomNInputs>(
            getTopBottomNValueExpr(state, accStmt, outputs),
            getTopBottomNSortByExpr(state, accStmt, outputs, SbExpr{specSlot}),
            SbExpr{specSlot});
    } else {
        // For all other accumulators, we call generateExpression() on 'argument' to create an
        // SbExpr and then we pass this SbExpr as the kInput arg to buildAddExprs().
        inputs = std::make_unique<AddSingleInput>(
            generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs));
    }

    return accOp.buildAddExprs(state, std::move(inputs));
}

boost::optional<std::vector<AccumInputsPtr>> generateAllAccumExprs(StageBuilderState& state,
                                                                   const GroupNode& groupNode,
                                                                   const PlanStageSlots& outputs) {
    boost::optional<std::vector<AccumInputsPtr>> accExprsVec;
    accExprsVec.emplace();

    for (const auto& accStmt : groupNode.accumulators) {
        // One accumulator may be translated to multiple accumulator expressions. For example, The
        // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
        // as sum(1).
        AccumInputsPtr accExprs = generateAccumExprs(state, accStmt, outputs);
        if (!accExprs) {
            return boost::none;
        }

        accExprsVec->emplace_back(std::move(accExprs));
    }

    return accExprsVec;
}

boost::optional<AddBlockExprs> generateAccumBlockExprs(StageBuilderState& state,
                                                       const AccumulationStatement& accStmt,
                                                       const PlanStageSlots& outputs) {
    auto accOp = AccumOp{accStmt};

    auto rootSlot = outputs.getResultObjIfExists();

    AccumInputsPtr inputs;

    // For $topN and $bottomN, we need to pass multiple SbExprs to buildAddExprs()
    // (an "input" expression and a "sortBy" expression).
    if (isTopBottomN(accStmt)) {
        auto specSlot = SbSlot{state.getSortSpecSlot(&accStmt)};

        inputs = std::make_unique<AddBlockTopBottomNInputs>(
            getBlockTopBottomNValueExpr(state, accStmt, outputs),
            getBlockTopBottomNSortByExpr(state, accStmt, outputs, SbExpr{specSlot}),
            SbExpr{specSlot});
    } else {
        // For all other accumulators, we call generateExpression() on 'argument' to create an
        // SbExpr and then we pass this SbExpr as the kInput arg to buildAddExprs().
        inputs = std::make_unique<AddSingleInput>(
            generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs));
    }

    return accOp.buildAddBlockExprs(state, std::move(inputs), outputs);
}

boost::optional<std::vector<AddBlockExprs>> generateAllAccumBlockExprs(
    StageBuilderState& state, const GroupNode& groupNode, const PlanStageSlots& outputs) {
    boost::optional<std::vector<AddBlockExprs>> blockAccumExprsVec;
    blockAccumExprsVec.emplace();

    for (const auto& accStmt : groupNode.accumulators) {
        // One accumulator may be translated to multiple accumulator expressions. For example, The
        // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
        // as sum(1).
        boost::optional<AddBlockExprs> blockAccumExprs =
            generateAccumBlockExprs(state, accStmt, outputs);

        if (!blockAccumExprs) {
            return boost::none;
        }

        blockAccumExprsVec->emplace_back(std::move(*blockAccumExprs));
    }

    return blockAccumExprsVec;
}

/**
 * This function generates one or more SbAggExprs for the specified accumulator ('accStmt')
 * and returns them.
 *
 * If 'genBlockAggs' is true, generateAccumAggs() accumulator may fail, in which case it
 * will leave the 'sbAggExprs' vector unmodified and return boost::none.
 */
boost::optional<SbAggExprVector> generateAccumAggs(StageBuilderState& state,
                                                   const AccumulationStatement& accStmt,
                                                   const PlanStageSlots& outputs,
                                                   AccumInputsPtr accExprs,
                                                   boost::optional<SbSlot> initRootSlot,
                                                   bool genBlockAggs,
                                                   boost::optional<SbSlot> bitmapInternalSlot) {
    SbExprBuilder b(state);

    auto accOp = AccumOp{accStmt};

    boost::optional<SbAggExprVector> sbAggExprs;
    sbAggExprs.emplace();

    // Generate the agg expressions (and blockAgg expressions too if 'genBlockAggs' is true).
    std::vector<BlockAggAndRowAgg> blockAggsAndRowAggs;

    if (!genBlockAggs) {
        // Handle the case where we only want to generate "normal" aggs without blockAggs.
        SbExpr::Vector aggs = accOp.buildAddAggs(state, std::move(accExprs));

        for (size_t i = 0; i < aggs.size(); ++i) {
            blockAggsAndRowAggs.emplace_back(BlockAggAndRowAgg{SbExpr{}, std::move(aggs[i])});
        }
    } else {
        // Handle the case where we want to generate aggs _and_ blockAggs.
        tassert(
            8448600, "Expected 'bitmapInternalSlot' to be defined", bitmapInternalSlot.has_value());

        boost::optional<std::vector<BlockAggAndRowAgg>> aggs =
            accOp.buildAddBlockAggs(state, std::move(accExprs), *bitmapInternalSlot);

        // If 'genBlockAggs' is true and we weren't able to generate block aggs for 'accStmt',
        // then we return boost::none to indicate failure.
        if (!aggs) {
            return boost::none;
        }

        blockAggsAndRowAggs = std::move(*aggs);
    }

    // Generate the init expressions.
    SbExpr::Vector inits = [&]() {
        PlanStageSlots slots;
        if (initRootSlot) {
            slots.setResultObj(*initRootSlot);
        }

        AccumInputsPtr initInputs;

        if (isAccumulatorN(accStmt)) {
            auto expr =
                generateExpression(state, accStmt.expr.initializer.get(), initRootSlot, slots);

            initInputs =
                std::make_unique<InitAccumNInputs>(std::move(expr), b.makeBoolConstant(true));
        }

        return accOp.buildInitialize(state, std::move(initInputs));
    }();

    tassert(7567301,
            "The accumulation and initialization expression should have the same length",
            inits.size() == blockAggsAndRowAggs.size());

    // For each 'init' / 'blockAgg' / 'agg' expression tuple, wrap the expressions in
    // an SbAggExpr and append the SbAggExpr to 'sbAggExprs'.
    for (size_t i = 0; i < blockAggsAndRowAggs.size(); i++) {
        SbExpr& init = inits[i];
        SbExpr& blockAgg = blockAggsAndRowAggs[i].blockAgg;
        SbExpr& rowAgg = blockAggsAndRowAggs[i].rowAgg;

        sbAggExprs->emplace_back(SbAggExpr{std::move(init), std::move(blockAgg), std::move(rowAgg)},
                                 boost::none);
    }

    return sbAggExprs;
}  // generateAccumAggs

/**
 * This function generates a vector of SbAggExprs that correspond to the accumulators from
 * the specified GroupNode ('groupNode') and returns it.
 *
 * If 'genBlockAggs' is true, generateAllAccumAggs() will fail if any of the accumulators do not
 * support block mode, in which case it will return boost::none.
 */
boost::optional<std::vector<SbAggExprVector>> generateAllAccumAggs(
    StageBuilderState& state,
    const GroupNode& groupNode,
    const PlanStageSlots& childOutputs,
    std::vector<AccumInputsPtr> accExprsVec,
    boost::optional<SbSlot> initRootSlot,
    bool genBlockAggs,
    boost::optional<SbSlot> bitmapInternalSlot) {
    // Loop over 'groupNode.accumulators' and populate 'sbAggExprs'.
    boost::optional<std::vector<SbAggExprVector>> sbAggExprs;
    sbAggExprs.emplace();

    size_t i = 0;
    for (const auto& accStmt : groupNode.accumulators) {
        boost::optional<SbAggExprVector> vec = generateAccumAggs(state,
                                                                 accStmt,
                                                                 childOutputs,
                                                                 std::move(accExprsVec[i]),
                                                                 initRootSlot,
                                                                 genBlockAggs,
                                                                 bitmapInternalSlot);

        // If we weren't able to generate block aggs for 'accStmt', then we return boost::none
        // to indicate failure. This should only happen when 'genBlockAggs' is true.
        if (!vec.has_value()) {
            return boost::none;
        }

        sbAggExprs->emplace_back(std::move(*vec));

        ++i;
    }

    return sbAggExprs;
}  // generateAllAccumAggs

/**
 * Generate a vector of (inputSlot, mergingExpression) pairs. The slot (whose id is allocated by
 * this function) will be used to store spilled partial aggregate values that have been recovered
 * from disk and deserialized. The merging expression is an agg function which combines these
 * partial aggregates.
 *
 * Usually the returned vector will be of length 1, but in some cases the MQL accumulation statement
 * is implemented by calculating multiple separate aggregates in the SBE plan, which are finalized
 * by a subsequent project stage to produce the ultimate value.
 */
SbExprSbSlotVector generateMergingExpressions(StageBuilderState& state,
                                              const AccumulationStatement& accStmt,
                                              int numInputSlots) {
    auto slotIdGenerator = state.slotIdGenerator;
    auto frameIdGenerator = state.frameIdGenerator;

    tassert(7039555, "'numInputSlots' must be positive", numInputSlots > 0);
    tassert(7039556, "expected non-null 'slotIdGenerator' pointer", slotIdGenerator);
    tassert(7039557, "expected non-null 'frameIdGenerator' pointer", frameIdGenerator);

    auto accOp = AccumOp{accStmt};

    SbSlotVector spillSlots;
    for (int i = 0; i < numInputSlots; ++i) {
        spillSlots.emplace_back(SbSlot{slotIdGenerator->generate()});
    }

    SbExpr::Vector mergingExprs = [&]() {
        AccumInputsPtr combineInputs;

        if (isTopBottomN(accStmt)) {
            auto sortSpec = SbExpr{SbSlot{state.getSortSpecSlot(&accStmt)}};
            combineInputs = std::make_unique<CombineAggsTopBottomNInputs>(std::move(sortSpec));
        }

        return accOp.buildCombineAggs(state, std::move(combineInputs), spillSlots);
    }();

    // Zip the slot vector and expression vector into a vector of pairs.
    tassert(7039550,
            "expected same number of slots and input exprs",
            spillSlots.size() == mergingExprs.size());
    SbExprSbSlotVector result;
    result.reserve(spillSlots.size());
    for (size_t i = 0; i < spillSlots.size(); ++i) {
        result.emplace_back(std::pair(std::move(mergingExprs[i]), spillSlots[i]));
    }
    return result;
}

/**
 * This function generates all of the merging expressions needed by the accumulators from the
 * specified GroupNode ('groupNode').
 */
std::vector<SbExprSbSlotVector> generateAllMergingExprs(StageBuilderState& state,
                                                        const GroupNode& groupNode) {
    // Since partial accumulator state may be spilled to disk and then merged, we must construct not
    // only the basic agg expressions for each accumulator, but also agg expressions that are used
    // to combine partial aggregates that have been spilled to disk.
    std::vector<SbExprSbSlotVector> mergingExprs;

    for (const auto& accStmt : groupNode.accumulators) {
        auto accOp = AccumOp{accStmt};
        size_t numAggs = accOp.getNumAggs();

        mergingExprs.emplace_back(generateMergingExpressions(state, accStmt, numAggs));
    }

    return mergingExprs;
}

/**
 * Helper to temporarily set a value in the current scope & reset it to its previous value on scope
 * exit.
 */
auto makeValueGuard(auto* dst, auto val) {
    return ScopeGuard{[dst, old = std::exchange(*dst, std::move(val))]() mutable {
        *dst = std::move(old);
    }};
}

/**
 * This function performs any computations needed after the HashAggStage (or BlockHashAggStage)
 * for the accumulators from 'groupNode'.
 *
 * generateGroupFinalStage() returns a tuple containing the updated SBE stage tree, a list of
 * output field names and a list of output field slots (corresponding to the accumulators from
 * 'groupNode'), and a new empty PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> generateGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    PlanStageSlots outputs,
    SbSlotVector& individualSlots,
    SbSlotVector groupBySlots,
    SbSlotVector groupOutSlots,
    const GroupNode& groupNode,
    bool idIsSingleKey,
    SbExpr idConstantValue) {
    // This group may be fully pushed down to execute on a shard; if so it will not be
    // merged on the router, and should emit the final agg results (not partial values).
    // Temporarily override `needsMerge` with the value for this particular group.
    const auto needsMergeGuard =
        makeValueGuard(&state.needsMerge, groupNode.willBeMerged && state.needsMerge);
    SbBuilder b(state, groupNode.nodeId());

    SbExpr idFinalExpr;

    if (idConstantValue) {
        // If '_id' is a constant, use the constant value for 'idExpr'.
        idFinalExpr = std::move(idConstantValue);
    } else if (idIsSingleKey) {
        // Otherwise, if '_id' is a single key, use the sole groupBy slot for 'idExpr'.
        idFinalExpr = SbExpr{groupBySlots[0]};
    } else {
        // Otherwise, create the appropriate "newObj(..)" expression and store it in 'idExpr'.
        const auto& idExpr = groupNode.groupByExpression;
        auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get());
        tassert(8620900, "Expected expression of type ExpressionObject", idExprObj != nullptr);

        std::vector<std::string> fieldNames;
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            fieldNames.emplace_back(fieldName);
        }

        SbExpr::Vector exprs;
        size_t i = 0;
        for (const auto& slot : groupBySlots) {
            exprs.emplace_back(b.makeStrConstant(fieldNames[i]));
            exprs.emplace_back(slot);
            ++i;
        }

        idFinalExpr = b.makeFunction("newObj"_sd, std::move(exprs));
    }

    const auto& accStmts = groupNode.accumulators;

    std::vector<SbSlotVector> aggSlotsVec;
    auto groupOutSlotsIt = groupOutSlots.begin();

    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        auto accOp = AccumOp{accStmts[idxAcc]};
        size_t numAggs = accOp.getNumAggs();

        aggSlotsVec.emplace_back(SbSlotVector(groupOutSlotsIt, groupOutSlotsIt + numAggs));
        groupOutSlotsIt += numAggs;
    }

    // Prepare to project 'idFinalExpr' to a slot.
    SbExprOptSbSlotVector projects;
    projects.emplace_back(std::move(idFinalExpr), boost::none);

    // Generate all the finalize expressions and prepare to project all these expressions
    // to slots.
    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = 0;
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        const AccumulationStatement& accStmt = accStmts[idxAcc];
        auto accOp = AccumOp{accStmt};

        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        AccumInputsPtr finalizeInputs;

        if (isTopBottomN(accStmt)) {
            auto sortSpec = SbExpr{SbSlot{state.getSortSpecSlot(&accStmt)}};
            finalizeInputs = std::make_unique<FinalizeTopBottomNInputs>(std::move(sortSpec));
        }

        SbExpr finalExpr =
            accOp.buildFinalize(state, std::move(finalizeInputs), aggSlotsVec[idxAcc]);

        // buildFinalize() might not return an expression if the final step is trivial.
        // For example, $first and $last's final steps are trivial.
        if (!finalExpr) {
            projects.emplace_back(groupOutSlots[idxAccFirstSlot], boost::none);
        } else {
            projects.emplace_back(std::move(finalExpr), boost::none);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Project all the aforementioned expressions to slots.
    auto [retStage, finalSlots] = b.makeProject(
        buildVariableTypes(outputs, individualSlots), std::move(groupStage), std::move(projects));

    individualSlots.insert(individualSlots.end(), finalSlots.begin(), finalSlots.end());

    return {std::move(retStage), std::move(fieldNames), std::move(finalSlots), std::move(outputs)};
}

/**
 * This function generates a HashAggStage or a BlockHashAggStage as appropriate for the specified
 * GroupNode ('groupNode').
 *
 * buildGroupAggregation() returns a tuple containing the updated SBE plan tree, the list of
 * slots corresponding to the group by inputs, and the list of accumulator output slots
 * corresponding to the accumulators from 'groupNode'.
 */
MONGO_COMPILER_NOINLINE
std::tuple<SbStage, SbSlotVector, SbSlotVector> buildGroupAggregation(
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector individualSlots,
    SbStage stage,
    SbExpr::Vector groupByExprs,
    std::vector<SbAggExprVector> sbAggExprs,
    std::vector<SbExprSbSlotVector> mergingExprs,
    bool useBlockHashAgg,
    std::vector<SbExpr::Vector> blockAccExprs,
    boost::optional<SbSlot> bitmapInternalSlot,
    const std::vector<SbSlotVector>& accumulatorDataSlots,
    PlanNodeId nodeId) {
    constexpr auto kBlockSelectivityBitmap = PlanStageSlots::kBlockSelectivityBitmap;

    SbBuilder b(state, nodeId);

    // Project the group by expressions and the accumulator arg expressions to slots.
    SbExprOptSbSlotVector projects;
    size_t numGroupByExprs = groupByExprs.size();

    for (auto& expr : groupByExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }

    for (auto& exprsVec : blockAccExprs) {
        for (auto& expr : exprsVec) {
            projects.emplace_back(std::move(expr), boost::none);
        }
    }

    auto [outStage, outSlots] = b.makeProject(
        buildVariableTypes(childOutputs, individualSlots), std::move(stage), std::move(projects));
    stage = std::move(outStage);

    SbSlotVector groupBySlots;
    SbSlotVector flattenedBlockAccArgSlots;
    SbSlotVector flattenedAccumulatorDataSlots;

    groupBySlots.reserve(numGroupByExprs);
    flattenedBlockAccArgSlots.reserve(outSlots.size() - numGroupByExprs);

    for (size_t i = 0; i < numGroupByExprs; ++i) {
        groupBySlots.emplace_back(outSlots[i]);
    }

    for (size_t i = numGroupByExprs; i < outSlots.size(); ++i) {
        flattenedBlockAccArgSlots.emplace_back(outSlots[i]);
    }

    for (const auto& slotsVec : accumulatorDataSlots) {
        flattenedAccumulatorDataSlots.insert(
            flattenedAccumulatorDataSlots.end(), slotsVec.begin(), slotsVec.end());
    }

    individualSlots.insert(individualSlots.end(), groupBySlots.begin(), groupBySlots.end());
    individualSlots.insert(
        individualSlots.end(), flattenedBlockAccArgSlots.begin(), flattenedBlockAccArgSlots.end());

    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto [hashAggStage, groupByOutSlots, aggSlots] = [&] {
        SbAggExprVector flattenedSbAggExprs;
        for (auto& vec : sbAggExprs) {
            std::move(vec.begin(), vec.end(), std::back_inserter(flattenedSbAggExprs));
        }

        SbExprSbSlotVector flattenedMergingExprs;
        for (auto& vec : mergingExprs) {
            std::move(vec.begin(), vec.end(), std::back_inserter(flattenedMergingExprs));
        }

        if (useBlockHashAgg) {
            tassert(8448603,
                    "Expected 'bitmapInternalSlot' to be defined",
                    bitmapInternalSlot.has_value());

            return b.makeBlockHashAgg(buildVariableTypes(childOutputs, individualSlots),
                                      std::move(stage),
                                      groupBySlots,
                                      std::move(flattenedSbAggExprs),
                                      childOutputs.get(kBlockSelectivityBitmap),
                                      flattenedBlockAccArgSlots,
                                      *bitmapInternalSlot,
                                      flattenedAccumulatorDataSlots,
                                      std::move(flattenedMergingExprs));
        } else {
            return b.makeHashAgg(buildVariableTypes(childOutputs, individualSlots),
                                 std::move(stage),
                                 groupBySlots,
                                 std::move(flattenedSbAggExprs),
                                 state.getCollatorSlot(),
                                 std::move(flattenedMergingExprs));
        }
    }();

    stage = std::move(hashAggStage);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}

/**
 * This function generates the kResult object at the end of $group when needed.
 */
std::pair<SbStage, SbSlot> generateGroupResultObject(SbStage stage,
                                                     StageBuilderState& state,
                                                     const GroupNode* groupNode,
                                                     const std::vector<std::string>& fieldNames,
                                                     const SbSlotVector& finalSlots) {
    SbBuilder b(state, groupNode->nodeId());

    SbExpr::Vector funcArgs;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        funcArgs.emplace_back(b.makeStrConstant(fieldNames[i]));
        funcArgs.emplace_back(finalSlots[i]);
    }

    StringData newObjFn = groupNode->shouldProduceBson ? "newBsonObj"_sd : "newObj"_sd;
    SbExpr outputExpr = b.makeFunction(newObjFn, std::move(funcArgs));

    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(outputExpr));
    stage = std::move(outStage);

    SbSlot slot = outSlots[0];
    slot.setTypeSignature(TypeSignature::kObjectType);

    return {std::move(stage), slot};
}

/**
 * This function generates the "root slot" for initializer expressions when it is needed.
 */
std::tuple<SbStage, SbExpr::Vector, SbSlot> generateInitRootSlot(
    SbStage stage,
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector& individualSlots,
    SbExpr::Vector groupByExprs,
    bool vectorizedGroupByExprs,
    ExpressionObject* idExprObj,
    boost::optional<SbSlot> slotIdForInitRoot,
    PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    bool idIsSingleKey = idExprObj == nullptr;

    // If there is more than one groupBy key, combine them all into a single object and
    // then use that object as sole groupBy key.
    if (!idIsSingleKey) {
        std::vector<std::string> fieldNames;
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            fieldNames.emplace_back(fieldName);
        }

        SbExpr::Vector exprs;
        size_t i = 0;
        for (const auto& e : groupByExprs) {
            exprs.emplace_back(b.makeStrConstant(fieldNames[i]));
            exprs.emplace_back(e.clone());
            ++i;
        }

        groupByExprs.clear();
        groupByExprs.emplace_back(b.makeFunction("newObj"_sd, std::move(exprs)));

        idIsSingleKey = true;
    }

    SbExpr& groupByExpr = groupByExprs[0];

    bool idIsKnownToBeObj = [&] {
        if (idExprObj != nullptr) {
            return true;
        } else if (groupByExpr.isConstantExpr() && !vectorizedGroupByExprs) {
            auto [tag, _] = groupByExpr.getConstantValue();
            return stage_builder::getTypeSignature(tag).isSubset(TypeSignature::kObjectType);
        }
        return false;
    }();

    // Project 'groupByExpr' to a slot.
    boost::optional<SbSlot> targetSlot = idIsKnownToBeObj ? slotIdForInitRoot : boost::none;
    auto [projectStage, projectOutSlots] =
        b.makeProject(buildVariableTypes(childOutputs),
                      std::move(stage),
                      std::pair(std::move(groupByExpr), targetSlot));
    stage = std::move(projectStage);

    groupByExpr = SbExpr{projectOutSlots[0]};
    individualSlots.emplace_back(projectOutSlots[0]);

    // As per the mql semantics add a project expression 'isObject(_id) ? _id : {}'
    // which will be provided as root to initializer expression.
    if (idIsKnownToBeObj) {
        // If we know '_id' is an object, then we can just use the slot as-is.
        return {std::move(stage), std::move(groupByExprs), projectOutSlots[0]};
    } else {
        // If we're not sure whether '_id' is an object, then we need to project the
        // aforementioned expression to a slot and use that.
        auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
        SbExpr idOrEmptyObjExpr = b.makeIf(b.makeFunction("isObject"_sd, groupByExpr.clone()),
                                           groupByExpr.clone(),
                                           b.makeConstant(emptyObjTag, emptyObjVal));

        auto [outStage, outSlots] =
            b.makeProject(buildVariableTypes(childOutputs, individualSlots),
                          std::move(stage),
                          SbExprOptSbSlotPair{std::move(idOrEmptyObjExpr), slotIdForInitRoot});
        stage = std::move(outStage);

        outSlots[0].setTypeSignature(TypeSignature::kObjectType);
        individualSlots.emplace_back(outSlots[0]);

        return {std::move(stage), std::move(groupByExprs), outSlots[0]};
    }
}
}  // namespace

/**
 * Translates a 'GroupNode' QSN into a sbe::PlanStage tree. This translation logic assumes that the
 * only child of the 'GroupNode' must return an Object (or 'BSONObject') and the translated sub-tree
 * must return 'BSONObject'. The returned 'BSONObject' will always have an "_id" field for the group
 * key and zero or more field(s) for accumulators.
 *
 * For example, a QSN tree: GroupNode(nodeId=2) over a CollectionScanNode(nodeId=1), we would have
 * the following translated sbe::PlanStage tree. In this example, we assume that the $group pipeline
 * spec is {"_id": "$a", "x": {"$min": "$b"}, "y": {"$first": "$b"}}.
 *
 * [2] mkbson s12 [_id = s8, x = s11, y = s10] true false
 * [2] project [s11 = (s9 ?: null)]
 * [2] group [s8] [s9 = min(
 *   let [
 *      l1.0 = s5
 *  ]
 *  in
 *      if (typeMatch(l1.0, 1088ll) ?: true)
 *      then Nothing
 *      else l1.0
 * ), s10 = first((s5 ?: null))]
 * [2] project [s8 = (s4 ?: null)]
 * [1] scan s6 s7 none none none none [s4 = a, s5 = b] @<collUuid> true false
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildGroup(const QuerySolutionNode* root,
                                                                     const PlanStageReqs& reqs) {
    tassert(6023414, "buildGroup() does not support kSortKey", !reqs.hasSortKeys());

    auto groupNode = static_cast<const GroupNode*>(root);

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    uassert(
        6360401,
        "GROUP cannot propagate a record id slot, but the record id was requested by the parent",
        !reqs.has(kRecordId));

    const auto& childNode = groupNode->children[0].get();

    // Builds the child and gets the child result slot. If the GroupNode doesn't need the full
    // result object, then we can process block values.
    auto childReqs = computeChildReqsForGroup(reqs, *groupNode);
    childReqs.setCanProcessBlockValues(!childReqs.hasResult());

    auto [childStage, childOutputs] = build(childNode, childReqs);
    auto stage = std::move(childStage);

    // Build the group stage in a separate helper method, so that the variables that are not needed
    // to setup the recursive call to build() don't consume precious stack.
    auto [outStage, fieldNames, finalSlots, outputs] =
        buildGroupImpl(std::move(stage), reqs, std::move(childOutputs), groupNode);
    stage = std::move(outStage);

    const std::vector<AccumulationStatement>& accStmts = groupNode->accumulators;

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    for (size_t i = 0; i < fieldNames.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fieldNames[i]), finalSlots[i]);
    }

    auto fieldNameSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    for (const auto& path : reqs.getFields()) {
        if (!fieldNameSet.count(getTopLevelField(path))) {
            auto nothingSlot = SbSlot{_state.getNothingSlot()};
            outputs.set(std::make_pair(PlanStageSlots::kField, path), nothingSlot);
        }
    }

    bool reqResultObj = reqs.hasResultObj();
    bool reqResultInfo = reqs.hasResultInfo();
    boost::optional<FieldEffects> effects;

    // If there is a ResultInfo req, check if this $group stage can participate with it.
    if (reqResultInfo) {
        const auto& reqTrackedFieldSet = reqs.getResultInfoTrackedFieldSet();
        const auto& reqEffects = reqs.getResultInfoEffects();

        // Get the effects of this $group stage.
        effects = getQsnInfo(root).effects;

        bool canParticipate = false;
        if (effects) {
            // Narrow 'effects' so that it only has effects applicable to fields in
            // 'reqTrackedFieldSet'.
            effects->narrow(reqTrackedFieldSet);

            if (auto composedEffects = composeEffectsForResultInfo(*effects, reqEffects)) {
                // If this group stage can participate with the result info req, then set
                // 'canParticipate' to true.
                canParticipate = true;
            }
        }

        if (!canParticipate) {
            // If this group stage cannot participate with the result info req, then we need to
            // produce a result object instead.
            reqResultObj = true;
            reqResultInfo = false;
        }
    }

    if (reqResultObj) {
        // Create a result object.
        auto [outStage, outSlot] =
            generateGroupResultObject(std::move(stage), _state, groupNode, fieldNames, finalSlots);
        stage = std::move(outStage);

        outputs.setResultObj(outSlot);
    } else if (reqResultInfo) {
        // Set the result base to be an empty object and add this group stage's effects to
        // the result info effects.
        outputs.setResultInfoBaseObj(SbSlot{_state.getEmptyObjSlot()});
        outputs.addEffectsToResultInfo(_state, reqs, *effects);
    }

    return {std::move(stage), std::move(outputs)};
}  // SlotBasedStageBuilder::buildGroup

/**
 * This function is called by buildGroup() and it contains most of the implementation for $group.
 *
 * It takes the GroupNode, the child's SBE stage tree, and the PlanStageSlots generated by the child
 * as input, and it returns a tuple containing the updated SBE stage tree, a list of output field
 * names and a list of output field slots (corresponding to the accumulators from the GroupNode),
 * and a new empty PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
SlotBasedStageBuilder::buildGroupImpl(SbStage stage,
                                      const PlanStageReqs& reqs,
                                      PlanStageSlots childOutputs,
                                      const GroupNode* groupNode) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool sbeFullEnabled = feature_flags::gFeatureFlagSbeFull.isEnabled(fcvSnapshot);
    const bool sbeBlockHashAggEnabled =
        feature_flags::gFeatureFlagSbeBlockHashAgg.isEnabled(fcvSnapshot);
    const bool featureFlagsAllowBlockHashAgg = sbeFullEnabled || sbeBlockHashAggEnabled;

    boost::optional<sbe::value::SlotId> collatorSlot = _state.getCollatorSlot();
    const auto& idExpr = groupNode->groupByExpression;
    const auto nodeId = groupNode->nodeId();
    SbBuilder b(_state, nodeId);

    tassert(5851601, "GROUP should have had group-by key expression", idExpr);

    {
        // Collect all the ExpressionFieldPaths referenced by from 'groupNode'.
        StringMap<const ExpressionFieldPath*> groupFieldMap = collectFieldPaths(groupNode);

        // Evaluate all of the ExpressionFieldPaths in 'groupFieldMap', project the results to
        // slots, and put the slots into 'childOutputs' as kPathExpr slots.
        stage = makePathExprsAvailableInSlots(
            _state, *groupNode, std::move(stage), childOutputs, groupFieldMap);
    }

    const std::vector<AccumulationStatement>& accs = groupNode->accumulators;

    // Check if any of the accumulators have a variable initializer.
    bool hasVariableGroupInit = false;
    for (const auto& accStmt : accs) {
        hasVariableGroupInit =
            hasVariableGroupInit || !ExpressionConstant::isNullOrConstant(accStmt.expr.initializer);
    }

    // Generate expressions for the group by keys.
    SbExpr::Vector groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);

    auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get());
    bool idIsSingleKey = idExprObj == nullptr;
    bool vectorizedGroupByExprs = false;

    if (childOutputs.hasBlockOutput()) {
        // Try to vectorize all the group keys.
        for (auto& sbExpr : groupByExprs) {
            sbExpr = buildVectorizedExpr(_state, std::move(sbExpr), childOutputs, false);
        }

        // If some expressions could not be vectorized, rebuild everything after transitioning to
        // scalar.
        if (std::any_of(groupByExprs.begin(), groupByExprs.end(), [](const SbExpr& expr) {
                return expr.isNull();
            })) {
            stage = buildBlockToRow(std::move(stage), _state, childOutputs);

            // buildBlockToRow() just made a bunch of changes to 'childOutputs', so we need
            // to re-generate 'groupByExprs'.
            groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);
        } else {
            vectorizedGroupByExprs = true;
        }
    }

    if (!vectorizedGroupByExprs) {
        // If we didn't vectorize the groupBy expressions call optimize() on them so that the call
        // to "isConstantExpr()" below can recognize more cases where the groupBy expr is constant.
        auto varTypes = buildVariableTypes(childOutputs);
        for (auto& sbExpr : groupByExprs) {
            sbExpr.optimize(_state, varTypes);
        }
    }

    // If one or more accumulators has a variable initializer, then we will eventually need
    // need to set up 'initRootSlot' later in this function.
    //
    // For now we just reserve a slot ID for 'initRootSlot' so that we can pass the slot ID to
    // generateAllAccumAggs(). Later we will make sure that 'initRootSlot' actually gets
    // populated.
    auto slotIdForInitRoot =
        hasVariableGroupInit ? boost::make_optional(SbSlot{_state.slotId()}) : boost::none;

    // The 'individualSlots' vector is used to keep track of all the slots that are currently
    // "active" that are not present in 'childOutputs'. This vector is used together with
    // 'childOutputs' when we need to do constant-folding / type analysis and vectorization.
    SbSlotVector individualSlots;

    // Define a helper lambda for checking if all accumulators support buildAddBlockAggs().
    auto canBuildBlockExprsAndBlockAggs = [&]() -> bool {
        return std::all_of(accs.begin(), accs.end(), [&](auto&& acc) {
            auto accOp = AccumOp{acc};
            return accOp.hasBuildAddBlockExprs() && accOp.hasBuildAddBlockAggs();
        });
    };

    // Below are the conditions for attempting to use BlockHashAggStage. When 'tryToUseBlockHashAgg'
    // is true, we will try vectorize to the accumulator args, and if that succeeds then we will try
    // to generate the block agg expressions. If all of that is successful, then we will set the
    // 'useBlockHashAgg' flag to true and we will use BlockHashAggStage. Otherwise, we use the
    // normal HashAggStage.
    const bool tryToUseBlockHashAgg = featureFlagsAllowBlockHashAgg &&
        childOutputs.hasBlockOutput() && !hasVariableGroupInit && !collatorSlot &&
        canBuildBlockExprsAndBlockAggs();

    bool useBlockHashAgg = false;

    boost::optional<std::vector<AccumInputsPtr>> accExprsVec;
    boost::optional<std::vector<SbAggExprVector>> sbAggExprs;
    std::vector<SbExpr::Vector> blockAccExprs;
    std::vector<SbSlotVector> accumulatorDataSlots;
    boost::optional<SbSlot> bitmapInternalSlot;

    if (tryToUseBlockHashAgg) {
        // If 'tryToUseBlockHashAgg' is true, then generate block arg expressions for all of the
        // accumulators.
        boost::optional<std::vector<AddBlockExprs>> accumBlockExprsVec =
            generateAllAccumBlockExprs(_state, *groupNode, childOutputs);

        // If generating block arg exprs for all the accumulators was successful, then proceed to
        // generating block agg expressions for all the accumulators.
        if (accumBlockExprsVec) {
            // Unpack 'accumBlockExprsVec' and populate 'accExprsVec', 'blockAccExprs', and
            // 'accumulatorDataSlots'.
            accExprsVec.emplace();

            for (auto& accumBlockExprs : *accumBlockExprsVec) {
                accExprsVec->emplace_back(std::move(accumBlockExprs.inputs));
                blockAccExprs.emplace_back(std::move(accumBlockExprs.exprs));
                accumulatorDataSlots.emplace_back(std::move(accumBlockExprs.slots));
            }

            // When calling generateAllAccumAggs() with genBlockAggs=true, we have to pass in two
            // additional "internal" slots.
            bitmapInternalSlot.emplace(SbSlot{_state.slotId()});

            // Generate the SbAggExprs for all the accumulators from 'groupNode'.
            sbAggExprs = generateAllAccumAggs(_state,
                                              *groupNode,
                                              childOutputs,
                                              std::move(*accExprsVec),
                                              slotIdForInitRoot,
                                              true /* genBlockAggs */,
                                              bitmapInternalSlot);
        }
    }

    if (sbAggExprs) {
        // If generating block agg expressions for all the accumulators was successful, then we
        // can use BlockHashAggStage.
        useBlockHashAgg = true;

        // Assert that the 'blockAgg' field is non-null for all SbAggExprs in 'sbAggExprs'.
        const bool hasNullBlockAggs =
            std::any_of(sbAggExprs->begin(), sbAggExprs->end(), [](auto&& v) {
                return std::any_of(
                    v.begin(), v.end(), [](auto&& e) { return e.first.blockAgg.isNull(); });
            });

        tassert(8751305, "Expected all blockAgg fields to be defined", !hasNullBlockAggs);
    }

    // If we aren't not going to use BlockHashAggStage, then we need to close the block processing
    // pipeline here.
    if (!useBlockHashAgg) {
        blockAccExprs.clear();
        accumulatorDataSlots.clear();
        bitmapInternalSlot = boost::none;

        if (childOutputs.hasBlockOutput()) {
            SbExprOptSbSlotVector projects;
            for (size_t i = 0; i < groupByExprs.size(); ++i) {
                projects.emplace_back(std::move(groupByExprs[i]), boost::none);
            }

            auto [projectStage, groupBySlots] = b.makeProject(
                buildVariableTypes(childOutputs), std::move(stage), std::move(projects));

            auto [outStage, outSlots] = buildBlockToRow(
                std::move(projectStage), _state, childOutputs, std::move(groupBySlots));
            stage = std::move(outStage);

            for (size_t i = 0; i < groupByExprs.size(); ++i) {
                groupByExprs[i] = outSlots[i];
            }

            individualSlots = outSlots;
        }
    }

    // If we didn't try to generate block agg expressions for the accumulators, or if we tried
    // and failed, then we need to generate scalar arg exprs and scalar agg expressions for all
    // the accumulators.
    if (!sbAggExprs) {
        // Generate the scalar arg exprs.
        accExprsVec = generateAllAccumExprs(_state, *groupNode, childOutputs);

        tassert(8751300, "Expected accumulator arg exprs to be defined", accExprsVec.has_value());

        // Generate the scalar agg expressions.
        sbAggExprs = generateAllAccumAggs(_state,
                                          *groupNode,
                                          childOutputs,
                                          std::move(*accExprsVec),
                                          slotIdForInitRoot,
                                          false /* genBlockAggs */,
                                          boost::none /* bitmapInternalSlot */);
    }

    tassert(8751301, "Expected accumulator aggs to be defined", sbAggExprs.has_value());

    // If one or more accumulators has a variable initializer, then we need to set up
    // 'initRootSlot'.
    boost::optional<SbSlot> initRootSlot;

    if (hasVariableGroupInit) {
        auto [outStage, outExprs, outSlot] = generateInitRootSlot(std::move(stage),
                                                                  _state,
                                                                  childOutputs,
                                                                  individualSlots,
                                                                  std::move(groupByExprs),
                                                                  vectorizedGroupByExprs,
                                                                  idExprObj,
                                                                  slotIdForInitRoot,
                                                                  nodeId);
        stage = std::move(outStage);
        groupByExprs = std::move(outExprs);
        initRootSlot.emplace(outSlot);

        idIsSingleKey = true;
    }

    // Generate merging expressions for all the accumulators.
    std::vector<SbExprSbSlotVector> mergingExprs = generateAllMergingExprs(_state, *groupNode);

    // If there is a single groupBy key that didn't get vectorized and is constant, and if none of
    // the accumulators had a variable initializer, then we set 'idConstantValue' and we clear the
    // the 'groupByExprs' vector.
    SbExpr idConstantValue;

    if (idIsSingleKey && !vectorizedGroupByExprs && groupByExprs[0].isConstantExpr() &&
        !hasVariableGroupInit) {
        idConstantValue = std::move(groupByExprs[0]);
        groupByExprs.clear();
    }

    // Build the HashAggStage or the BlockHashAggStage.
    auto [outStage, groupByOutSlots, aggOutSlots] =
        buildGroupAggregation(_state,
                              childOutputs,
                              std::move(individualSlots),
                              std::move(stage),
                              std::move(groupByExprs),
                              std::move(*sbAggExprs),
                              std::move(mergingExprs),
                              useBlockHashAgg,
                              std::move(blockAccExprs),
                              bitmapInternalSlot,
                              accumulatorDataSlots,
                              nodeId);
    stage = std::move(outStage);

    // Initialize a new PlanStageSlots object ('outputs').
    PlanStageSlots outputs;

    // After the HashAgg/BlockHashAgg stage, the only slots that are "active" are the group-by slots
    // ('groupByOutSlots') and the output slots for the accumulators from groupNode ('aggOutSlots').
    individualSlots = groupByOutSlots;
    individualSlots.insert(individualSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

    if (useBlockHashAgg) {
        tassert(8448606,
                "Expected at least one group by slot or agg out slot",
                !groupByOutSlots.empty() || !aggOutSlots.empty());

        // This stage re-maps the selectivity bitset slot.
        outputs.set(PlanStageSlots::kBlockSelectivityBitmap,
                    childOutputs.get(PlanStageSlots::kBlockSelectivityBitmap));
    }

    // For now we unconditionally end the block processing pipeline here.
    if (outputs.hasBlockOutput()) {
        auto hashAggOutSlots = groupByOutSlots;
        hashAggOutSlots.insert(hashAggOutSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

        auto [outStage, blockToRowOutSlots] =
            buildBlockToRow(std::move(stage), _state, outputs, std::move(hashAggOutSlots));
        stage = std::move(outStage);

        for (size_t i = 0; i < groupByOutSlots.size(); ++i) {
            groupByOutSlots[i] = blockToRowOutSlots[i];
        }
        for (size_t i = 0; i < aggOutSlots.size(); ++i) {
            size_t blockToRowOutSlotsIdx = groupByOutSlots.size() + i;
            aggOutSlots[i] = blockToRowOutSlots[blockToRowOutSlotsIdx];
        }

        // buildBlockToRow() just made a bunch of changes to 'groupByOutSlots' and 'aggOutSlots',
        // so we need to re-generate 'individualSlots'.
        individualSlots = groupByOutSlots;
        individualSlots.insert(individualSlots.end(), aggOutSlots.begin(), aggOutSlots.end());
    }

    // Builds the final stage(s) over the collected accumulators.
    return generateGroupFinalStage(_state,
                                   std::move(stage),
                                   std::move(outputs),
                                   individualSlots,
                                   std::move(groupByOutSlots),
                                   std::move(aggOutSlots),
                                   *groupNode,
                                   idIsSingleKey,
                                   std::move(idConstantValue));
}  // SlotBasedStageBuilder::buildGroupImpl
}  // namespace mongo::stage_builder
