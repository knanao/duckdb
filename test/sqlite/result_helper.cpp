#include "result_helper.hpp"
#include "termcolor.hpp"
#include "re2/re2.h"
#include "duckdb/parser/parser.hpp"
#include "catch.hpp"
#include "sqllogic_test_runner.hpp"
#include "duckdb/common/crypto/md5.hpp"
#include "test_helpers.hpp"

#include <thread>

namespace duckdb {

TestResultHelper::TestResultHelper(Command &command, MaterializedQueryResult &result_p)
    : runner(command.runner), result(result_p), file_name(command.file_name), query_line(command.query_line),
      sql_query(command.sql_query) {
}

TestResultHelper::TestResultHelper(Query &query, MaterializedQueryResult &result_p)
    : TestResultHelper((Command &)query, result_p) {
	query_ptr = &query;
}

TestResultHelper::TestResultHelper(Statement &stmt, MaterializedQueryResult &result_p)
    : TestResultHelper((Command &)stmt, result_p) {
	expect_ok = stmt.expect_ok;
}

void TestResultHelper::CheckQueryResult(unique_ptr<MaterializedQueryResult> owned_result) {
	D_ASSERT(query_ptr);
	auto &expected_column_count = query_ptr->expected_column_count;
	auto &values = query_ptr->values;
	auto &sort_style = query_ptr->sort_style;
	auto &query_has_label = query_ptr->query_has_label;
	auto &query_label = query_ptr->query_label;

	if (!result.success) {
		PrintLineSep();
		std::cerr << "Query unexpectedly failed (" << file_name.c_str() << ":" << query_line << ")\n";
		PrintLineSep();
		PrintSQL(sql_query);
		PrintLineSep();
		PrintHeader("Actual result:");
		result.Print();
		if (SkipErrorMessage(result.error)) {
			runner.finished_processing_file = true;
			return;
		}
		FAIL_LINE(file_name, query_line, 0);
	}
	idx_t row_count = result.collection.Count();
	idx_t column_count = result.ColumnCount();
	idx_t total_value_count = row_count * column_count;
	bool compare_hash =
	    query_has_label || (runner.hash_threshold > 0 && total_value_count > idx_t(runner.hash_threshold));
	bool result_is_hash = false;
	// check if the current line (the first line of the result) is a hash value
	if (values.size() == 1 && ResultIsHash(values[0])) {
		compare_hash = true;
		result_is_hash = true;
	}

	vector<string> result_values_string;
	DuckDBConvertResult(result, runner.original_sqlite_test, result_values_string);
	if (runner.output_result_mode) {
		// names
		for (idx_t c = 0; c < result.ColumnCount(); c++) {
			if (c != 0) {
				std::cerr << "\t";
			}
			std::cerr << result.names[c];
		}
		std::cerr << std::endl;
		// types
		for (idx_t c = 0; c < result.ColumnCount(); c++) {
			if (c != 0) {
				std::cerr << "\t";
			}
			std::cerr << result.types[c].ToString();
		}
		std::cerr << std::endl;
		PrintLineSep();
		for (idx_t r = 0; r < result.collection.Count(); r++) {
			for (idx_t c = 0; c < result.ColumnCount(); c++) {
				if (c != 0) {
					std::cerr << "\t";
				}
				std::cerr << result_values_string[r * result.ColumnCount() + c];
			}
			std::cerr << std::endl;
		}
	}

	// perform any required query sorts
	if (sort_style == SortStyle::ROW_SORT) {
		// row-oriented sorting
		idx_t ncols = result.ColumnCount();
		idx_t nrows = total_value_count / ncols;
		vector<vector<string>> rows;
		rows.reserve(nrows);
		for (idx_t row_idx = 0; row_idx < nrows; row_idx++) {
			vector<string> row;
			row.reserve(ncols);
			for (idx_t col_idx = 0; col_idx < ncols; col_idx++) {
				row.push_back(move(result_values_string[row_idx * ncols + col_idx]));
			}
			rows.push_back(move(row));
		}
		// sort the individual rows
		std::sort(rows.begin(), rows.end(), [](const vector<string> &a, const vector<string> &b) {
			for (idx_t col_idx = 0; col_idx < a.size(); col_idx++) {
				if (a[col_idx] != b[col_idx]) {
					return a[col_idx] < b[col_idx];
				}
			}
			return false;
		});

		// now reconstruct the values from the rows
		for (idx_t row_idx = 0; row_idx < nrows; row_idx++) {
			for (idx_t col_idx = 0; col_idx < ncols; col_idx++) {
				result_values_string[row_idx * ncols + col_idx] = move(rows[row_idx][col_idx]);
			}
		}
	} else if (sort_style == SortStyle::VALUE_SORT) {
		// sort values independently
		std::sort(result_values_string.begin(), result_values_string.end());
	}

	vector<string> comparison_values;
	if (values.size() == 1 && ResultIsFile(values[0])) {
		comparison_values =
		    LoadResultFromFile(SQLLogicTestRunner::LoopReplacement(values[0], runner.running_loops), result.names);
	} else {
		comparison_values = values;
	}

	// compute the hash of the results if there is a hash label or we are past the hash threshold
	string hash_value;
	if (runner.output_hash_mode || compare_hash) {
		MD5Context context;
		for (idx_t i = 0; i < total_value_count; i++) {
			context.Add(result_values_string[i]);
			context.Add("\n");
		}
		string digest = context.FinishHex();
		hash_value = to_string(total_value_count) + " values hashing to " + digest;
		if (runner.output_hash_mode) {
			PrintLineSep();
			PrintSQL(sql_query);
			PrintLineSep();
			std::cerr << hash_value << std::endl;
			PrintLineSep();
			return;
		}
	}

	if (!compare_hash) {
		// check if the row/column count matches
		idx_t original_expected_columns = expected_column_count;
		bool column_count_mismatch = false;
		if (expected_column_count != result.ColumnCount()) {
			// expected column count is different from the count found in the result
			// we try to keep going with the number of columns in the result
			expected_column_count = result.ColumnCount();
			column_count_mismatch = true;
		}
		idx_t expected_rows = comparison_values.size() / expected_column_count;
		// we first check the counts: if the values are equal to the amount of rows we expect the results to be row-wise
		bool row_wise = expected_column_count > 1 && comparison_values.size() == result.collection.Count();
		if (!row_wise) {
			// the counts do not match up for it to be row-wise
			// however, this can also be because the query returned an incorrect # of rows
			// we make a guess: if everything contains tabs, we still treat the input as row wise
			bool all_tabs = true;
			for (auto &val : comparison_values) {
				if (val.find('\t') == string::npos) {
					all_tabs = false;
					break;
				}
			}
			row_wise = all_tabs;
		}
		if (row_wise) {
			// values are displayed row-wise, format row wise with a tab
			expected_rows = comparison_values.size();
			row_wise = true;
		} else if (comparison_values.size() % expected_column_count != 0) {
			if (column_count_mismatch) {
				ColumnCountMismatch(original_expected_columns, row_wise);
			}
			PrintErrorHeader("Error in test!");
			PrintLineSep();
			fprintf(stderr, "Expected %d columns, but %d values were supplied\n", (int)expected_column_count,
			        (int)comparison_values.size());
			fprintf(stderr, "This is not cleanly divisible (i.e. the last row does not have enough values)\n");
			FAIL_LINE(file_name, query_line, 0);
		}
		if (expected_rows != result.collection.Count()) {
			if (column_count_mismatch) {
				ColumnCountMismatch(original_expected_columns, row_wise);
			}
			PrintErrorHeader("Wrong row count in query!");
			std::cerr << "Expected " << termcolor::bold << expected_rows << termcolor::reset << " rows, but got "
			          << termcolor::bold << result.collection.Count() << termcolor::reset << " rows" << std::endl;
			PrintLineSep();
			PrintSQL(sql_query);
			PrintLineSep();
			PrintResultError(result, comparison_values, expected_column_count, row_wise);
			FAIL_LINE(file_name, query_line, 0);
		}

		if (row_wise) {
			idx_t current_row = 0;
			for (idx_t i = 0; i < total_value_count && i < comparison_values.size(); i++) {
				// split based on tab character
				auto splits = StringUtil::Split(comparison_values[i], "\t");
				if (splits.size() != expected_column_count) {
					if (column_count_mismatch) {
						ColumnCountMismatch(original_expected_columns, row_wise);
					}
					PrintLineSep();
					PrintErrorHeader("Error in test! Column count mismatch after splitting on tab!");
					std::cerr << "Expected " << termcolor::bold << expected_column_count << termcolor::reset
					          << " columns, but got " << termcolor::bold << splits.size() << termcolor::reset
					          << " columns" << std::endl;
					std::cerr << "Does the result contain tab values? In that case, place every value on a single row."
					          << std::endl;
					PrintLineSep();
					PrintSQL(sql_query);
					PrintLineSep();
					FAIL_LINE(file_name, query_line, 0);
				}
				for (idx_t c = 0; c < splits.size(); c++) {
					bool success = CompareValues(result_values_string[current_row * expected_column_count + c],
					                             splits[c], current_row, c, comparison_values, expected_column_count,
					                             row_wise, result_values_string);
					if (!success) {
						FAIL_LINE(file_name, query_line, 0);
					}
					// we do this just to increment the assertion counter
					REQUIRE(success);
				}
				current_row++;
			}
		} else {
			idx_t current_row = 0, current_column = 0;
			for (idx_t i = 0; i < total_value_count && i < comparison_values.size(); i++) {
				bool success = CompareValues(result_values_string[current_row * expected_column_count + current_column],
				                             comparison_values[i], current_row, current_column, comparison_values,
				                             expected_column_count, row_wise, result_values_string);
				if (!success) {
					FAIL_LINE(file_name, query_line, 0);
				}
				// we do this just to increment the assertion counter
				REQUIRE(success);

				current_column++;
				if (current_column == expected_column_count) {
					current_row++;
					current_column = 0;
				}
			}
		}
		if (column_count_mismatch) {
			PrintLineSep();
			PrintErrorHeader("Wrong column count in query!");
			std::cerr << "Expected " << termcolor::bold << original_expected_columns << termcolor::reset
			          << " columns, but got " << termcolor::bold << expected_column_count << termcolor::reset
			          << " columns" << std::endl;
			PrintLineSep();
			PrintSQL(sql_query);
			PrintLineSep();
			std::cerr << "The expected result " << termcolor::bold << "matched" << termcolor::reset
			          << " the query result." << std::endl;
			std::cerr << termcolor::bold << "Suggested fix: modify header to \"" << termcolor::green << "query "
			          << string(result.ColumnCount(), 'I') << termcolor::reset << termcolor::bold << "\""
			          << termcolor::reset << std::endl;
			PrintLineSep();
			FAIL_LINE(file_name, query_line, 0);
		}
	} else {
		bool hash_compare_error = false;
		if (query_has_label) {
			// the query has a label: check if the hash has already been computed
			auto entry = runner.hash_label_map.find(query_label);
			if (entry == runner.hash_label_map.end()) {
				// not computed yet: add it tot he map
				runner.hash_label_map[query_label] = hash_value;
				runner.result_label_map[query_label] = move(owned_result);
			} else {
				hash_compare_error = entry->second != hash_value;
			}
		}
		if (result_is_hash) {
			D_ASSERT(values.size() == 1);
			hash_compare_error = values[0] != hash_value;
		}
		if (hash_compare_error) {
			PrintErrorHeader("Wrong result hash!");
			PrintLineSep();
			PrintSQL(sql_query);
			PrintLineSep();
			PrintHeader("Expected result:");
			PrintLineSep();
			if (runner.result_label_map.find(query_label) != runner.result_label_map.end()) {
				runner.result_label_map[query_label]->Print();
			} else {
				std::cerr << "???" << std::endl;
			}
			PrintHeader("Actual result:");
			PrintLineSep();
			result.Print();
			FAIL_LINE(file_name, query_line, 0);
		}
		REQUIRE(!hash_compare_error);
	}
}

void TestResultHelper::CheckStatementResult() {
	bool error = !result.success;

	if (runner.output_result_mode || runner.debug_mode) {
		result.Print();
	}

	/* Check to see if we are expecting success or failure */
	if (!expect_ok) {
		// even in the case of "statement error", we do not accept ALL errors
		// internal errors are never expected
		// neither are "unoptimized result differs from original result" errors
		bool internal_error = TestIsInternalError(result.error);
		if (!internal_error) {
			error = !error;
		} else {
			expect_ok = true;
		}
	}

	/* Report an error if the results do not match expectation */
	if (error) {
		PrintErrorHeader(!expect_ok ? "Query unexpectedly succeeded!" : "Query unexpectedly failed!");
		PrintLineSep();
		PrintSQL(sql_query);
		PrintLineSep();
		result.Print();
		if (expect_ok && SkipErrorMessage(result.error)) {
			runner.finished_processing_file = true;
			return;
		}
		FAIL_LINE(file_name, query_line, 0);
	}
	REQUIRE(!error);
}

vector<string> TestResultHelper::LoadResultFromFile(string fname, vector<string> names) {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("PRAGMA threads=" + to_string(std::thread::hardware_concurrency()));
	fname = StringUtil::Replace(fname, "<FILE>:", "");

	string struct_definition = "STRUCT_PACK(";
	for (idx_t i = 0; i < names.size(); i++) {
		if (i > 0) {
			struct_definition += ", ";
		}
		struct_definition += "\"" + names[i] + "\" := 'VARCHAR'";
	}
	struct_definition += ")";

	auto csv_result =
	    con.Query("SELECT * FROM read_csv('" + fname + "', header=1, sep='|', columns=" + struct_definition + ")");
	if (!csv_result->success) {
		string error = StringUtil::Format("Could not read CSV File \"%s\": %s", fname, csv_result->error);
		PrintErrorHeader(error.c_str());
		FAIL_LINE(file_name, query_line, 0);
	}
	query_ptr->expected_column_count = csv_result->ColumnCount();

	vector<string> values;
	while (true) {
		auto chunk = csv_result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
				values.push_back(chunk->GetValue(c, r).ToString());
			}
		}
	}
	return values;
}

void TestResultHelper::PrintExpectedResult(vector<string> &values, idx_t columns, bool row_wise) {
	if (row_wise) {
		for (idx_t r = 0; r < values.size(); r++) {
			fprintf(stderr, "%s\n", values[r].c_str());
		}
	} else {
		idx_t c = 0;
		for (idx_t r = 0; r < values.size(); r++) {
			if (c != 0) {
				fprintf(stderr, "\t");
			}
			fprintf(stderr, "%s", values[r].c_str());
			c++;
			if (c >= columns) {
				fprintf(stderr, "\n");
				c = 0;
			}
		}
	}
}

bool TestResultHelper::SkipErrorMessage(const string &message) {
	if (StringUtil::Contains(message, "HTTP")) {
		return true;
	}
	if (StringUtil::Contains(message, "Unable to connect")) {
		return true;
	}
	return false;
}

string TestResultHelper::SQLLogicTestConvertValue(Value value, LogicalType sql_type, bool original_sqlite_test) {
	if (value.is_null) {
		return "NULL";
	} else {
		if (original_sqlite_test) {
			// sqlite test hashes want us to convert floating point numbers to integers
			switch (sql_type.id()) {
			case LogicalTypeId::DECIMAL:
			case LogicalTypeId::FLOAT:
			case LogicalTypeId::DOUBLE:
				return value.CastAs(LogicalType::BIGINT).ToString();
			default:
				break;
			}
		}
		switch (sql_type.id()) {
		case LogicalTypeId::BOOLEAN:
			return value.value_.boolean ? "1" : "0";
		default: {
			string str = value.ToString();
			if (str.empty()) {
				return "(empty)";
			} else {
				return str;
			}
		}
		}
	}
}

// standard result conversion: one line per value
void TestResultHelper::DuckDBConvertResult(MaterializedQueryResult &result, bool original_sqlite_test,
                                           vector<string> &out_result) {
	size_t r, c;
	idx_t row_count = result.collection.Count();
	idx_t column_count = result.ColumnCount();

	out_result.resize(row_count * column_count);
	for (r = 0; r < row_count; r++) {
		for (c = 0; c < column_count; c++) {
			auto value = result.GetValue(c, r);
			auto converted_value = SQLLogicTestConvertValue(value, result.types[c], original_sqlite_test);
			out_result[r * column_count + c] = converted_value;
		}
	}
}

void TestResultHelper::PrintLineSep() {
	string line_sep = string(80, '=');
	std::cerr << termcolor::color<128, 128, 128> << line_sep << termcolor::reset << std::endl;
}

void TestResultHelper::PrintHeader(string header) {
	std::cerr << termcolor::bold << header << termcolor::reset << std::endl;
}

void TestResultHelper::PrintSQL(string sql) {
	std::cerr << termcolor::bold << "SQL Query" << termcolor::reset << std::endl;
	auto tokens = Parser::Tokenize(sql);
	for (idx_t i = 0; i < tokens.size(); i++) {
		auto &token = tokens[i];
		idx_t next = i + 1 < tokens.size() ? tokens[i + 1].start : sql.size();
		// adjust the highlighting based on the type
		switch (token.type) {
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
			std::cerr << termcolor::yellow;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
			std::cerr << termcolor::green << termcolor::bold;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
			std::cerr << termcolor::grey;
			break;
		}
		// print the current token
		std::cerr << sql.substr(token.start, next - token.start);
		// reset and move to the next token
		std::cerr << termcolor::reset;
	}
	std::cerr << std::endl;
}

void TestResultHelper::PrintErrorHeader(const char *description) {
	PrintLineSep();
	std::cerr << termcolor::red << termcolor::bold << description << " " << termcolor::reset;
	std::cerr << termcolor::bold << "(" << file_name << ":" << query_line << ")!" << termcolor::reset << std::endl;
}

void TestResultHelper::PrintResultError(vector<string> &result_values, vector<string> &values,
                                        idx_t expected_column_count, bool row_wise) {
	PrintHeader("Expected result:");
	PrintLineSep();
	PrintExpectedResult(values, expected_column_count, row_wise);
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintExpectedResult(result_values, expected_column_count, false);
}

void TestResultHelper::PrintResultError(MaterializedQueryResult &result, vector<string> &values,
                                        idx_t expected_column_count, bool row_wise) {
	PrintHeader("Expected result:");
	PrintLineSep();
	PrintExpectedResult(values, expected_column_count, row_wise);
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	result.Print();
}

bool TestResultHelper::ResultIsHash(const string &result) {
	idx_t pos = 0;
	// first parse the rows
	while (result[pos] >= '0' && result[pos] <= '9') {
		pos++;
	}
	if (pos == 0) {
		return false;
	}
	string constant_str = " values hashing to ";
	string example_hash = "acd848208cc35c7324ece9fcdd507823";
	if (pos + constant_str.size() + example_hash.size() != result.size()) {
		return false;
	}
	if (result.substr(pos, constant_str.size()) != constant_str) {
		return false;
	}
	pos += constant_str.size();
	// now parse the hash
	while ((result[pos] >= '0' && result[pos] <= '9') || (result[pos] >= 'a' && result[pos] <= 'z')) {
		pos++;
	}
	return pos == result.size();
}

bool TestResultHelper::ResultIsFile(string result) {
	return StringUtil::StartsWith(result, "<FILE>:");
}

bool TestResultHelper::CompareValues(string lvalue_str, string rvalue_str, idx_t current_row, idx_t current_column,
                                     vector<string> &values, idx_t expected_column_count, bool row_wise,
                                     vector<string> &result_values) {
	Value lvalue, rvalue;
	bool error = false;
	// simple first test: compare string value directly
	if (lvalue_str == rvalue_str) {
		return true;
	}
	if (StringUtil::StartsWith(rvalue_str, "<REGEX>:") || StringUtil::StartsWith(rvalue_str, "<!REGEX>:")) {
		bool want_match = StringUtil::StartsWith(rvalue_str, "<REGEX>:");
		string regex_str = StringUtil::Replace(StringUtil::Replace(rvalue_str, "<REGEX>:", ""), "<!REGEX>:", "");
		RE2::Options options;
		options.set_dot_nl(true);
		RE2 re(regex_str, options);
		if (!re.ok()) {
			PrintErrorHeader("Test error!");
			PrintLineSep();
			std::cerr << termcolor::red << termcolor::bold << "Failed to parse regex: " << re.error()
			          << termcolor::reset << std::endl;
			PrintLineSep();
			return false;
		}
		bool regex_matches = RE2::FullMatch(lvalue_str, re);
		if (regex_matches == want_match) {
			return true;
		}
	}
	// some times require more checking (specifically floating point numbers because of inaccuracies)
	// if not equivalent we need to cast to the SQL type to verify
	auto sql_type = result.types[current_column];
	if (sql_type.IsNumeric()) {
		bool converted_lvalue = false;
		try {
			if (lvalue_str == "NULL") {
				lvalue = Value(sql_type);
			} else {
				lvalue = Value(lvalue_str);
				if (!lvalue.TryCastAs(sql_type)) {
					return false;
				}
			}
			converted_lvalue = true;
			if (rvalue_str == "NULL") {
				rvalue = Value(sql_type);
			} else {
				rvalue = Value(rvalue_str);
				if (!rvalue.TryCastAs(sql_type)) {
					return false;
				}
			}
			error = !Value::ValuesAreEqual(lvalue, rvalue);
		} catch (std::exception &ex) {
			PrintErrorHeader("Test error!");
			PrintLineSep();
			PrintSQL(sql_query);
			PrintLineSep();
			std::cerr << termcolor::red << termcolor::bold << "Cannot convert value "
			          << (converted_lvalue ? rvalue_str : lvalue_str) << " to type " << sql_type.ToString()
			          << termcolor::reset << std::endl;
			std::cerr << termcolor::red << termcolor::bold << ex.what() << termcolor::reset << std::endl;
			PrintLineSep();
			return false;
		}
	} else if (sql_type == LogicalType::BOOLEAN) {
		auto low_r_val = StringUtil::Lower(rvalue_str);
		auto low_l_val = StringUtil::Lower(lvalue_str);

		string true_str = "true";
		string false_str = "false";
		if (low_l_val == true_str || lvalue_str == "1") {
			lvalue = Value(1);
		} else if (low_l_val == false_str || lvalue_str == "0") {
			lvalue = Value(0);
		}
		if (low_r_val == true_str || rvalue_str == "1") {
			rvalue = Value(1);
		} else if (low_r_val == false_str || rvalue_str == "0") {
			rvalue = Value(0);
		}
		error = !Value::ValuesAreEqual(lvalue, rvalue);

	} else {
		// for other types we just mark the result as incorrect
		error = true;
	}
	if (error) {
		PrintErrorHeader("Wrong result in query!");
		PrintLineSep();
		PrintSQL(sql_query);
		PrintLineSep();
		std::cerr << termcolor::red << termcolor::bold << "Mismatch on row " << current_row + 1 << ", column "
		          << current_column + 1 << std::endl
		          << termcolor::reset;
		std::cerr << lvalue_str << " <> " << rvalue_str << std::endl;
		PrintLineSep();
		PrintResultError(result_values, values, expected_column_count, row_wise);
		return false;
	}
	return true;
}

void TestResultHelper::ColumnCountMismatch(idx_t expected_column_count, bool row_wise) {
	PrintErrorHeader("Wrong column count in query!");
	std::cerr << "Expected " << termcolor::bold << expected_column_count << termcolor::reset << " columns, but got "
	          << termcolor::bold << result.ColumnCount() << termcolor::reset << " columns" << std::endl;
	PrintLineSep();
	PrintSQL(sql_query);
	PrintLineSep();
	PrintResultError(result, query_ptr->values, expected_column_count, row_wise);
	FAIL_LINE(file_name, query_line, 0);
}

} // namespace duckdb
