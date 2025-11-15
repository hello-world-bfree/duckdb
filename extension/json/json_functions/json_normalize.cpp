#include "json_executors.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"

namespace duckdb {

//! Normalize a JSON object by ensuring all keys are present (filling missing ones with NULL)
//! This function takes a JSON object and a list of expected keys, returning a struct with all keys
static void NormalizeJSONWithKeys(yyjson_val *json_obj, const vector<string> &expected_keys,
                                   Vector &result, idx_t result_idx, yyjson_alc *alc) {
	D_ASSERT(yyjson_is_obj(json_obj));

	// Build a map of existing keys in the JSON object
	json_key_map_t<yyjson_val *> key_values;

	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(json_obj, idx, max, key, val) {
		const auto key_ptr = unsafe_yyjson_get_str(key);
		const auto key_len = unsafe_yyjson_get_len(key);
		key_values[{key_ptr, key_len}] = val;
	}

	// Create a struct with all expected keys
	auto &struct_children = StructVector::GetEntries(result);
	auto &struct_validity = FlatVector::Validity(result);

	for (idx_t key_idx = 0; key_idx < expected_keys.size(); key_idx++) {
		auto &child_vector = *struct_children[key_idx];
		auto &child_validity = FlatVector::Validity(child_vector);

		const auto &expected_key = expected_keys[key_idx];
		JSONKey lookup_key{expected_key.c_str(), expected_key.size()};

		auto it = key_values.find(lookup_key);
		if (it == key_values.end()) {
			// Key not found - set to NULL
			child_validity.SetInvalid(result_idx);
		} else {
			// Key found - extract value as JSON string
			auto child_data = FlatVector::GetData<string_t>(child_vector);
			child_data[result_idx] = JSONCommon::WriteVal<yyjson_val>(it->second, alc);
		}
	}
}

//! Merge multiple JSON structure nodes into a single unified schema
static JSONStructureNode MergeAllSchemas(const vector<yyjson_val *> &json_values, bool ignore_errors) {
	JSONStructureNode merged;

	for (auto *val : json_values) {
		if (val && !unsafe_yyjson_is_null(val)) {
			JSONStructure::ExtractStructure(val, merged, ignore_errors);
		}
	}

	return merged;
}

//! Extract all keys from a merged JSON structure node
static vector<string> ExtractKeysFromStructure(const JSONStructureNode &node) {
	vector<string> keys;

	if (node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::STRUCT) {
		const auto &desc = node.descriptions[0];
		for (const auto &child : desc.children) {
			if (child.key) {
				keys.push_back(*child.key);
			}
		}
	}

	return keys;
}

//! Scan a DataChunk of JSON values to extract a unified schema
static unique_ptr<FunctionData> BindNormalizeSchema(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	// The actual schema detection will happen at runtime
	// Return a JSON type for now
	bound_function.return_type = LogicalType::JSON();
	return nullptr;
}

//! Normalize JSON values to have consistent structure
static void JSONNormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	auto &input = args.data[0];
	const idx_t count = args.size();

	// First pass: collect all JSON values and build merged schema
	vector<yyjson_val *> json_values;
	json_values.reserve(count);

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(count, input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	// Parse all JSON documents
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (input_data.validity.RowIsValid(idx)) {
			auto doc = JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, alc);
			json_values.push_back(doc->root);
		} else {
			json_values.push_back(nullptr);
		}
	}

	// Merge all schemas
	auto merged_schema = MergeAllSchemas(json_values, true);
	auto all_keys = ExtractKeysFromStructure(merged_schema);

	if (all_keys.empty()) {
		// No consistent schema found, return original JSON
		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [](string_t input) {
			return input;
		});
		return;
	}

	// Convert merged schema to a LogicalType
	LogicalType normalized_type;
	try {
		normalized_type = JSONStructure::StructureToType(context, merged_schema,
		                                                  NumericLimits<idx_t>::Maximum(),
		                                                  1.0, NumericLimits<idx_t>::Maximum(),
		                                                  0, LogicalTypeId::SQLNULL);
	} catch (...) {
		// Schema inference failed, return original JSON
		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [](string_t input) {
			return input;
		});
		return;
	}

	// Transform each JSON value to have all keys
	// For simplicity, we'll return a JSON representation with all keys
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!json_values[i] || unsafe_yyjson_is_null(json_values[i])) {
			result_validity.SetInvalid(i);
			continue;
		}

		if (!yyjson_is_obj(json_values[i])) {
			// Not an object, return as-is
			result_data[i] = inputs[input_data.sel->get_index(i)];
			continue;
		}

		// Create a new JSON object with all keys
		auto doc = yyjson_mut_doc_new(alc);
		auto obj = yyjson_mut_obj(doc);

		// Build map of existing values
		json_key_map_t<yyjson_val *> existing_values;
		size_t idx, max;
		yyjson_val *key, *val;
		yyjson_obj_foreach(json_values[i], idx, max, key, val) {
			const auto key_ptr = unsafe_yyjson_get_str(key);
			const auto key_len = unsafe_yyjson_get_len(key);
			existing_values[{key_ptr, key_len}] = val;
		}

		// Add all expected keys
		for (const auto &expected_key : all_keys) {
			JSONKey lookup_key{expected_key.c_str(), expected_key.size()};
			auto it = existing_values.find(lookup_key);

			yyjson_mut_val *mut_val;
			if (it == existing_values.end()) {
				// Missing key - add NULL
				mut_val = yyjson_mut_null(doc);
			} else {
				// Copy existing value
				mut_val = yyjson_val_mut_copy(doc, it->second);
			}

			yyjson_mut_obj_add(obj, yyjson_mut_strn(doc, expected_key.c_str(), expected_key.size()), mut_val);
		}

		result_data[i] = JSONCommon::WriteVal<yyjson_mut_val>(obj, alc);
	}

	JSONAllocator::AddBuffer(result, alc);
}

ScalarFunctionSet JSONFunctions::GetNormalizeFunction() {
	ScalarFunctionSet set("json_normalize");
	set.AddFunction(ScalarFunction({LogicalType::JSON()}, LogicalType::JSON(), JSONNormalizeFunction,
	                               nullptr, nullptr, nullptr, JSONFunctionLocalState::Init));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::JSON(), JSONNormalizeFunction,
	                               nullptr, nullptr, nullptr, JSONFunctionLocalState::Init));
	return set;
}

} // namespace duckdb
