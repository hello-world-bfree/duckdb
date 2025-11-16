#include "json_executors.hpp"
#include <set>

namespace duckdb {

static string_t JSONNormalize(yyjson_val *val, yyjson_alc *alc, Vector &, ValidityMask &, idx_t) {
	D_ASSERT(alc);

	// Only normalize arrays of objects
	if (!unsafe_yyjson_is_arr(val)) {
		// For non-arrays, just return as-is
		size_t len_size_t;
		auto data = yyjson_val_write_opts(val, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
		idx_t len = len_size_t;
		return string_t(data, len);
	}

	// First pass: collect all unique keys from all objects in the array
	std::set<string> all_keys;
	size_t arr_idx, arr_max;
	yyjson_val *arr_val;
	yyjson_arr_foreach(val, arr_idx, arr_max, arr_val) {
		if (unsafe_yyjson_is_obj(arr_val)) {
			size_t obj_idx, obj_max;
			yyjson_val *key, *obj_val;
			yyjson_obj_foreach(arr_val, obj_idx, obj_max, key, obj_val) {
				all_keys.insert(unsafe_yyjson_get_str(key));
			}
		}
	}

	// If no keys found or array is empty, return as-is
	if (all_keys.empty()) {
		size_t len_size_t;
		auto data = yyjson_val_write_opts(val, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
		idx_t len = len_size_t;
		return string_t(data, len);
	}

	// Second pass: create normalized objects with all keys
	yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(alc);
	yyjson_mut_val *result_arr = yyjson_mut_arr(mut_doc);
	yyjson_mut_doc_set_root(mut_doc, result_arr);

	yyjson_arr_foreach(val, arr_idx, arr_max, arr_val) {
		if (unsafe_yyjson_is_obj(arr_val)) {
			yyjson_mut_val *new_obj = yyjson_mut_obj(mut_doc);

			// Add all keys in sorted order, using null for missing keys
			for (const auto &key_str : all_keys) {
				yyjson_mut_val *mut_key = yyjson_mut_strcpy(mut_doc, key_str.c_str());
				yyjson_val *existing_val = yyjson_obj_get(arr_val, key_str.c_str());
				yyjson_mut_val *mut_val;
				if (existing_val) {
					mut_val = yyjson_val_mut_copy(mut_doc, existing_val);
				} else {
					mut_val = yyjson_mut_null(mut_doc);
				}
				yyjson_mut_obj_add(new_obj, mut_key, mut_val);
			}

			yyjson_mut_arr_append(result_arr, new_obj);
		} else {
			// Non-object elements are kept as-is
			yyjson_mut_val *copied = yyjson_val_mut_copy(mut_doc, arr_val);
			yyjson_mut_arr_append(result_arr, copied);
		}
	}

	// Write the result
	size_t len_size_t;
	auto data = yyjson_mut_write_opts(mut_doc, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
	idx_t len = len_size_t;
	return string_t(data, len);
}

static void JSONNormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto json_type = args.data[0].GetType();
	D_ASSERT(json_type == LogicalType::VARCHAR || json_type == LogicalType::JSON());

	JSONExecutors::UnaryExecute<string_t>(args, state, result, JSONNormalize);
}

static void GetJSONNormalizeFunctionInternal(ScalarFunctionSet &set, const LogicalType &json) {
	set.AddFunction(ScalarFunction("json_normalize", {json}, LogicalType::JSON(), JSONNormalizeFunction, nullptr,
	                               nullptr, nullptr, JSONFunctionLocalState::Init));
}

ScalarFunctionSet JSONFunctions::GetNormalizeFunction() {
	ScalarFunctionSet set("json_normalize");
	GetJSONNormalizeFunctionInternal(set, LogicalType::JSON());
	return set;
}

} // namespace duckdb
