#include "json_executors.hpp"

namespace duckdb {

static void NormalizeObject(yyjson_val *val, const string &prefix, yyjson_mut_doc *mut_doc, yyjson_mut_val *result_obj) {
	size_t idx, max;
	yyjson_val *key, *child_val;
	yyjson_obj_foreach(val, idx, max, key, child_val) {
		string new_key = prefix.empty() ? unsafe_yyjson_get_str(key) : prefix + "." + unsafe_yyjson_get_str(key);
		if (unsafe_yyjson_is_obj(child_val)) {
			NormalizeObject(child_val, new_key, mut_doc, result_obj);
		} else {
			yyjson_mut_val *mut_key = yyjson_mut_strcpy(mut_doc, new_key.c_str());
			yyjson_mut_val *mut_val = yyjson_val_mut_copy(mut_doc, child_val);
			yyjson_mut_obj_add(result_obj, mut_key, mut_val);
		}
	}
}

static string_t JSONNormalize(yyjson_val *val, yyjson_alc *alc, Vector &, ValidityMask &, idx_t) {
	D_ASSERT(alc);

	if (!unsafe_yyjson_is_obj(val)) {
		// For non-objects, just return as-is
		size_t len_size_t;
		auto data = yyjson_val_write_opts(val, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
		idx_t len = len_size_t;
		return string_t(data, len);
	}

	// Create a mutable document for the result
	yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(alc);
	yyjson_mut_val *result_obj = yyjson_mut_obj(mut_doc);
	yyjson_mut_doc_set_root(mut_doc, result_obj);

	// Normalize the object
	NormalizeObject(val, "", mut_doc, result_obj);

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
