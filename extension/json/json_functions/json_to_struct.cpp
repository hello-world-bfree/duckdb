#include "json_common.hpp"
#include "json_functions.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

struct JSONToStructBindData : public TableFunctionData {
	vector<string> json_strings;  // Store as string (owns memory)
	LogicalType result_type;
	vector<string> all_keys;
};

struct JSONToStructGlobalState : public GlobalTableFunctionState {
	JSONToStructGlobalState() : current_row(0) {
	}

	idx_t current_row;
	mutex lock;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static yyjson_mut_val *ConvertToMutable(yyjson_val *val, yyjson_mut_doc *doc) {
	if (!val) {
		return yyjson_mut_null(doc);
	}

	switch (yyjson_get_type(val)) {
	case YYJSON_TYPE_NULL:
		return yyjson_mut_null(doc);
	case YYJSON_TYPE_BOOL:
		return yyjson_mut_bool(doc, unsafe_yyjson_get_bool(val));
	case YYJSON_TYPE_NUM:
		if (unsafe_yyjson_is_sint(val)) {
			return yyjson_mut_sint(doc, unsafe_yyjson_get_sint(val));
		} else if (unsafe_yyjson_is_uint(val)) {
			return yyjson_mut_uint(doc, unsafe_yyjson_get_uint(val));
		} else {
			return yyjson_mut_real(doc, unsafe_yyjson_get_real(val));
		}
	case YYJSON_TYPE_STR:
		return yyjson_mut_strn(doc, unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
	case YYJSON_TYPE_ARR: {
		auto arr = yyjson_mut_arr(doc);
		yyjson_val *child;
		yyjson_arr_iter iter;
		yyjson_arr_iter_init(val, &iter);
		while ((child = yyjson_arr_iter_next(&iter))) {
			yyjson_mut_arr_append(arr, ConvertToMutable(child, doc));
		}
		return arr;
	}
	case YYJSON_TYPE_OBJ: {
		auto obj = yyjson_mut_obj(doc);
		yyjson_val *key, *child;
		yyjson_obj_iter iter;
		yyjson_obj_iter_init(val, &iter);
		while ((key = yyjson_obj_iter_next(&iter))) {
			child = yyjson_obj_iter_get_val(key);
			yyjson_mut_obj_add(obj, yyjson_mut_strn(doc, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key)),
			                   ConvertToMutable(child, doc));
		}
		return obj;
	}
	default:
		return yyjson_mut_null(doc);
	}
}

static vector<string> ExtractKeysFromStructure(const JSONStructureNode &node) {
	vector<string> keys;
	if (node.descriptions.empty()) {
		return keys;
	}

	for (const auto &desc : node.descriptions) {
		if (desc.first == JSONCommon::TYPE_STRING_OBJECT && desc.second.children) {
			for (const auto &child : *desc.second.children) {
				keys.push_back(child.first);
			}
		}
	}
	return keys;
}

static JSONStructureNode MergeAllSchemas(const vector<yyjson_val *> &values, bool ignore_errors) {
	JSONStructureNode merged_schema;
	for (auto val : values) {
		if (val && yyjson_is_obj(val)) {
			JSONStructure::ExtractStructure(val, merged_schema, ignore_errors);
		}
	}
	return merged_schema;
}

static unique_ptr<FunctionData> JSONToStructBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<JSONToStructBindData>();

	// The input is the JSON data passed as a parameter
	// We need to materialize it to scan for schema
	if (input.inputs.empty()) {
		throw BinderException("json_to_struct requires at least one argument");
	}

	auto &json_input = input.inputs[0];
	if (json_input.type().id() != LogicalTypeId::LIST) {
		throw BinderException("json_to_struct expects a LIST of JSON values");
	}

	// Get the list of JSON values
	auto &list_value = ListValue::GetChildren(json_input);

	JSONAllocator json_allocator(Allocator::DefaultAllocator());
	auto alc = json_allocator.GetYYAlc();

	// Parse all JSON documents and merge schemas
	vector<yyjson_val *> json_values;
	vector<yyjson_doc *> docs;

	for (const auto &val : list_value) {
		if (val.IsNull()) {
			json_values.push_back(nullptr);
			docs.push_back(nullptr);
			result->json_strings.push_back("");  // Empty string for NULL
		} else {
			auto str = val.GetValue<string>();
			result->json_strings.push_back(str);  // Store as std::string
			auto doc = JSONCommon::ReadDocument(string_t(str), JSONCommon::READ_FLAG, alc);
			docs.push_back(doc);
			json_values.push_back(doc ? doc->root : nullptr);
		}
	}

	// Merge all schemas
	auto merged_schema = MergeAllSchemas(json_values, true);
	result->all_keys = ExtractKeysFromStructure(merged_schema);

	// If no keys found, return empty struct
	if (result->all_keys.empty()) {
		result->result_type = LogicalType::STRUCT({});
	} else {
		// Infer types from merged schema
		result->result_type = JSONStructure::StructureToType(context, merged_schema, NumericLimits<idx_t>::Maximum(),
		                                                     1.0, NumericLimits<idx_t>::Maximum(), 0);
	}

	// Return single column named 'data' with the inferred STRUCT type
	names.push_back("data");
	return_types.push_back(result->result_type);

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> JSONToStructInitGlobal(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<JSONToStructGlobalState>();
}

static void JSONToStructFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<JSONToStructBindData>();
	auto &global_state = data_p.global_state->Cast<JSONToStructGlobalState>();

	lock_guard<mutex> glock(global_state.lock);

	idx_t count = 0;
	const idx_t max_count = STANDARD_VECTOR_SIZE;

	JSONAllocator json_allocator(BufferAllocator::Get(context));
	auto alc = json_allocator.GetYYAlc();

	// Process rows
	while (global_state.current_row < bind_data.json_strings.size() && count < max_count) {
		auto &json_str = bind_data.json_strings[global_state.current_row];

		if (json_str.empty()) {
			// NULL value
			FlatVector::SetNull(output.data[0], count, true);
		} else {
			// Parse and normalize
			auto doc = JSONCommon::ReadDocument(string_t(json_str), JSONCommon::READ_FLAG, alc);

			if (!doc || !doc->root || !yyjson_is_obj(doc->root)) {
				FlatVector::SetNull(output.data[0], count, true);
			} else {
				// Create normalized JSON with all keys
				auto mut_doc = yyjson_mut_doc_new(alc);
				auto obj = yyjson_mut_obj(mut_doc);
				yyjson_mut_doc_set_root(mut_doc, obj);

				for (const auto &expected_key : bind_data.all_keys) {
					auto existing_val = yyjson_obj_getn(doc->root, expected_key.c_str(), expected_key.size());
					yyjson_mut_val *mut_val;
					if (existing_val) {
						mut_val = ConvertToMutable(existing_val, mut_doc);
					} else {
						mut_val = yyjson_mut_null(mut_doc);
					}
					yyjson_mut_obj_add(obj, yyjson_mut_strn(mut_doc, expected_key.c_str(), expected_key.size()),
					                   mut_val);
				}

				// Convert to string and transform to STRUCT
				auto normalized_json = JSONCommon::WriteVal<yyjson_mut_val>(obj, alc);

				// Now transform this normalized JSON to STRUCT
				auto transform_doc = JSONCommon::ReadDocument(normalized_json, JSONCommon::READ_FLAG, alc);
				if (transform_doc && transform_doc->root) {
					JSONTransformOptions options(false, false, false, false);
					auto vals = JSONCommon::AllocateArray<yyjson_val *>(alc, 1);
					vals[0] = transform_doc->root;

					// Create temporary result for single value
					Vector temp_result(bind_data.result_type, 1);
					JSONTransform::Transform(vals, alc, temp_result, 1, options, nullptr);

					// Copy to output
					VectorOperations::Copy(temp_result, output.data[0], 1, 0, count);
				} else {
					FlatVector::SetNull(output.data[0], count, true);
				}
			}
		}

		global_state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

void JSONFunctions::RegisterJSONToStructFunction(ExtensionLoader &loader) {
	TableFunction json_to_struct("json_to_struct", {LogicalType::LIST(LogicalType::JSON())}, JSONToStructFunction,
	                             JSONToStructBind, JSONToStructInitGlobal);

	// Also accept VARCHAR list
	TableFunction json_to_struct_varchar("json_to_struct", {LogicalType::LIST(LogicalType::VARCHAR)},
	                                     JSONToStructFunction, JSONToStructBind, JSONToStructInitGlobal);

	loader.RegisterFunction(json_to_struct);
	loader.RegisterFunction(json_to_struct_varchar);
}

} // namespace duckdb
