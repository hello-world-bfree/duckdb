#include "json_executors.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"

namespace duckdb {

//! json_infer_type: Infers STRUCT type from JSON data by sampling
//! Returns a VARCHAR describing the struct type (can be used with json_transform)
static void JSONInferTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	auto &input = args.data[0];
	const idx_t count = args.size();

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(count, input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	// Collect schemas from all rows
	JSONStructureNode merged_schema;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			continue;
		}

		try {
			auto doc = JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, alc);
			if (doc && doc->root) {
				JSONStructure::ExtractStructure(doc->root, merged_schema, true);
			}
		} catch (...) {
			continue;
		}
	}

	// Convert to LogicalType
	LogicalType inferred_type;
	try {
		inferred_type = JSONStructure::StructureToType(state.GetContext(), merged_schema,
		                                               NumericLimits<idx_t>::Maximum(), 1.0,
		                                               NumericLimits<idx_t>::Maximum(), 0, LogicalTypeId::SQLNULL);
	} catch (...) {
		// Return NULL if inference fails
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true);
		return;
	}

	// Convert LogicalType to string representation
	auto type_str = inferred_type.ToString();

	// Return as constant since type is same for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	*result_data = StringVector::AddString(result, type_str);
}

ScalarFunctionSet JSONFunctions::GetInferTypeFunction() {
	ScalarFunctionSet set("json_infer_type");
	set.AddFunction(ScalarFunction({LogicalType::JSON()}, LogicalType::VARCHAR, JSONInferTypeFunction, nullptr,
	                               nullptr, nullptr, JSONFunctionLocalState::Init));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, JSONInferTypeFunction, nullptr,
	                               nullptr, nullptr, JSONFunctionLocalState::Init));
	return set;
}

//! json_to_struct: Convenience function combining infer + transform
//! Automatically infers schema and transforms JSON to STRUCT
static void JSONToStructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	auto &input = args.data[0];
	const idx_t count = args.size();

	// First pass: infer schema
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(count, input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	JSONStructureNode merged_schema;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			continue;
		}

		try {
			auto doc = JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, alc);
			if (doc && doc->root) {
				JSONStructure::ExtractStructure(doc->root, merged_schema, true);
			}
		} catch (...) {
			continue;
		}
	}

	// Convert to LogicalType
	LogicalType struct_type;
	try {
		struct_type = JSONStructure::StructureToType(state.GetContext(), merged_schema,
		                                             NumericLimits<idx_t>::Maximum(), 1.0,
		                                             NumericLimits<idx_t>::Maximum(), 0, LogicalTypeId::SQLNULL);
	} catch (...) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true);
		return;
	}

	// Now use json_transform to convert data
	// This is a simplified version - full implementation would call json_transform's logic
	JSONTransform::TransformFromJSON(input, result, count, struct_type, alc);
}

ScalarFunctionSet JSONFunctions::GetToStructFunction() {
	ScalarFunctionSet set("json_to_struct");
	set.AddFunction(ScalarFunction({LogicalType::JSON()}, LogicalType::STRUCT({}), JSONToStructFunction, nullptr,
	                               nullptr, nullptr, JSONFunctionLocalState::Init));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::STRUCT({}), JSONToStructFunction, nullptr,
	                               nullptr, nullptr, JSONFunctionLocalState::Init));
	return set;
}

} // namespace duckdb
