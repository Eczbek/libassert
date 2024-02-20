#ifndef _CRT_SECURE_NO_WARNINGS
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp)
#define _CRT_SECURE_NO_WARNINGS // done only for strerror
#endif
#include <assert/assert.hpp>

// Copyright (c) 2021-2024 Jeremy Rifkin under the MIT license
// https://github.com/jeremy-rifkin/libassert

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <string_view>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cpptrace/cpptrace.hpp>

#include "common.hpp"
#include "utils.hpp"
#include "analysis.hpp"

#define IS_WINDOWS 0

#if defined(_WIN32)
 #undef IS_WINDOWS
 #define IS_WINDOWS 1
 #ifndef STDIN_FILENO
  #define STDIN_FILENO _fileno(stdin)
  #define STDOUT_FILENO _fileno(stdout)
  #define STDERR_FILENO _fileno(stderr)
 #endif
 #include <windows.h>
 #include <io.h>
 #undef min // fucking windows headers, man
 #undef max
#elif defined(__linux) || defined(__APPLE__) || defined(__unix__)
 #include <sys/ioctl.h>
 #include <unistd.h>
 // NOLINTNEXTLINE(misc-include-cleaner)
 #include <climits> // MAX_PATH
#else
 #error "no"
#endif

#if LIBASSERT_IS_MSVC
 // wchar -> char string warning
 #pragma warning(disable : 4244)
#endif

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace libassert {
    // https://stackoverflow.com/questions/23369503/get-size-of-terminal-window-rows-columns
    LIBASSERT_ATTR_COLD int terminal_width(int fd) {
        if(fd < 0) {
            return 0;
        }
        #if IS_WINDOWS
         DWORD windows_handle = small_static_map(fd).lookup(
             STDIN_FILENO, STD_INPUT_HANDLE,
             STDOUT_FILENO, STD_OUTPUT_HANDLE,
             STDERR_FILENO, STD_ERROR_HANDLE
         );
         CONSOLE_SCREEN_BUFFER_INFO csbi;
         HANDLE h = GetStdHandle(windows_handle);
         if(h == INVALID_HANDLE_VALUE) { return 0; }
         if(!GetConsoleScreenBufferInfo(h, &csbi)) { return 0; }
         return csbi.srWindow.Right - csbi.srWindow.Left + 1;
        #else
         struct winsize w;
         // NOLINTNEXTLINE(misc-include-cleaner)
         if(ioctl(fd, TIOCGWINSZ, &w) == -1) { return 0; }
         return w.ws_col;
        #endif
    }
}

namespace libassert::detail {

    /*
     * system wrappers
     */

    LIBASSERT_ATTR_COLD LIBASSERT_EXPORT void enable_virtual_terminal_processing_if_needed() {
        // enable colors / ansi processing if necessary
        #if IS_WINDOWS
         // https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#example-of-enabling-virtual-terminal-processing
         #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
          constexpr DWORD ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x4;
         #endif
         HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
         DWORD dwMode = 0;
         if(hOut == INVALID_HANDLE_VALUE) return;
         if(!GetConsoleMode(hOut, &dwMode)) return;
         if(dwMode != (dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
         if(!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
        #endif
    }

    LIBASSERT_ATTR_COLD static bool isatty(int fd) {
        #if IS_WINDOWS
         return _isatty(fd);
        #else
         return ::isatty(fd);
        #endif
    }

    // NOTE: Not thread-safe. Must be called in a thread-safe manner.
    LIBASSERT_ATTR_COLD std::string strerror_wrapper(int e) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        return strerror(e);
    }

    LIBASSERT_ATTR_COLD
    opaque_trace::~opaque_trace() {
        delete static_cast<cpptrace::raw_trace*>(trace);
    }

    LIBASSERT_ATTR_COLD
    opaque_trace get_stacktrace_opaque() {
        return {new cpptrace::raw_trace(cpptrace::generate_raw_trace())};
    }

    /*
     * stack trace printing
     */

    struct column_t {
        size_t width;
        std::vector<highlight_block> blocks;
        bool right_align = false;
        LIBASSERT_ATTR_COLD column_t(size_t _width, std::vector<highlight_block> _blocks, bool _right_align = false)
                                               : width(_width), blocks(std::move(_blocks)), right_align(_right_align) {}
        LIBASSERT_ATTR_COLD column_t(const column_t&) = default;
        LIBASSERT_ATTR_COLD column_t(column_t&&) = default;
        LIBASSERT_ATTR_COLD ~column_t() = default;
        LIBASSERT_ATTR_COLD column_t& operator=(const column_t&) = default;
        LIBASSERT_ATTR_COLD column_t& operator=(column_t&&) = default;
    };

    using path_components = std::vector<std::string>;

    LIBASSERT_ATTR_COLD
    static path_components parse_path(const std::string_view path) {
        #if IS_WINDOWS
         constexpr std::string_view path_delim = "/\\";
        #else
         constexpr std::string_view path_delim = "/";
        #endif
        // Some cases to consider
        // projects/libassert/demo.cpp               projects   libassert  demo.cpp
        // /glibc-2.27/csu/../csu/libc-start.c  /  glibc-2.27 csu      libc-start.c
        // ./demo.exe                           .  demo.exe
        // ./../demo.exe                        .. demo.exe
        // ../x.hpp                             .. x.hpp
        // /foo/./x                                foo        x
        // /foo//x                                 f          x
        path_components parts;
        for(const std::string& part : split(path, path_delim)) {
            if(parts.empty()) {
                // first gets added no matter what
                parts.push_back(part);
            } else {
                if(part.empty()) {
                    // nop
                } else if(part == ".") {
                    // nop
                } else if(part == "..") {
                    // cases where we have unresolvable ..'s, e.g. ./../../demo.exe
                    if(parts.back() == "." || parts.back() == "..") {
                        parts.push_back(part);
                    } else {
                        parts.pop_back();
                    }
                } else {
                    parts.push_back(part);
                }
            }
        }
        LIBASSERT_PRIMITIVE_ASSERT(!parts.empty());
        LIBASSERT_PRIMITIVE_ASSERT(parts.back() != "." && parts.back() != "..");
        return parts;
    }

    class path_trie {
        // Backwards path trie structure
        // e.g.:
        // a/b/c/d/e     disambiguate to -> c/d/e
        // a/b/f/d/e     disambiguate to -> f/d/e
        //  2   2   1   1   1
        // e - d - c - b - a
        //      \   1   1   1
        //       \ f - b - a
        // Nodes are marked with the number of downstream branches
        size_t downstream_branches = 1;
        std::string root;
        std::unordered_map<std::string, std::unique_ptr<path_trie>> edges;
    public:
        LIBASSERT_ATTR_COLD
        explicit path_trie(std::string _root) : root(std::move(_root)) {};
        LIBASSERT_ATTR_COLD
        void insert(const path_components& path) {
            LIBASSERT_PRIMITIVE_ASSERT(path.back() == root);
            insert(path, (int)path.size() - 2);
        }
        LIBASSERT_ATTR_COLD
        path_components disambiguate(const path_components& path) {
            path_components result;
            path_trie* current = this;
            LIBASSERT_PRIMITIVE_ASSERT(path.back() == root);
            result.push_back(current->root);
            for(size_t i = path.size() - 2; i >= 1; i--) {
                LIBASSERT_PRIMITIVE_ASSERT(current->downstream_branches >= 1);
                if(current->downstream_branches == 1) {
                    break;
                }
                const std::string& component = path[i];
                LIBASSERT_PRIMITIVE_ASSERT(current->edges.count(component));
                current = current->edges.at(component).get();
                result.push_back(current->root);
            }
            std::reverse(result.begin(), result.end());
            return result;
        }
    private:
        LIBASSERT_ATTR_COLD
        void insert(const path_components& path, int i) {
            if(i < 0) {
                return;
            }
            if(!edges.count(path[i])) {
                if(!edges.empty()) {
                    downstream_branches++; // this is to deal with making leaves have count 1
                }
                edges.insert({path[i], std::make_unique<path_trie>(path[i])});
            }
            downstream_branches -= edges.at(path[i])->downstream_branches;
            edges.at(path[i])->insert(path, i - 1);
            downstream_branches += edges.at(path[i])->downstream_branches;
        }
    };

    LIBASSERT_ATTR_COLD
    // TODO
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    static std::string wrapped_print(const std::vector<column_t>& columns, color_scheme scheme) {
        // 2d array rows/columns
        struct line_content {
            size_t length;
            std::string content;
        };
        std::vector<std::vector<line_content>> lines;
        lines.emplace_back(columns.size());
        // populate one column at a time
        for(size_t i = 0; i < columns.size(); i++) {
            auto [width, blocks, _] = columns[i];
            size_t current_line = 0;
            for(auto& block : blocks) {
                size_t block_i = 0;
                // digest block
                while(block_i != block.content.size()) {
                    if(lines.size() == current_line) {
                        lines.emplace_back(columns.size());
                    }
                    // number of characters we can extract from the block
                    size_t extract = std::min(width - lines[current_line][i].length, block.content.size() - block_i);
                    LIBASSERT_PRIMITIVE_ASSERT(block_i + extract <= block.content.size());
                    auto substr = std::string_view(block.content).substr(block_i, extract);
                    // handle newlines
                    if(auto x = substr.find('\n'); x != std::string_view::npos) {
                        substr = substr.substr(0, x);
                        extract = x + 1; // extract newline but don't print
                    }
                    // append
                    lines[current_line][i].content += block.color;
                    lines[current_line][i].content += substr;
                    lines[current_line][i].content += block.color.empty() ? "" : scheme.reset;
                    // advance
                    block_i += extract;
                    lines[current_line][i].length += extract;
                    // new line if necessary
                    // substr.size() != extract iff newline
                    if(lines[current_line][i].length >= width || substr.size() != extract) {
                        current_line++;
                    }
                }
            }
        }
        // print
        std::string output;
        for(auto& line : lines) {
            // don't print empty columns with no content in subsequent columns and more importantly
            // don't print empty spaces they'll mess up lines after terminal resizing even more
            size_t last_col = 0;
            for(size_t i = 0; i < line.size(); i++) {
                if(!line[i].content.empty()) {
                    last_col = i;
                }
            }
            for(size_t i = 0; i <= last_col; i++) {
                auto& content = line[i];
                if(columns[i].right_align) {
                    output += stringf("%-*s%s%s",
                                      i == last_col ? 0 : int(columns[i].width - content.length), "",
                                      content.content.c_str(),
                                      i == last_col ? "\n" : " ");
                } else {
                    output += stringf("%s%-*s%s",
                                      content.content.c_str(),
                                      i == last_col ? 0 : int(columns[i].width - content.length), "",
                                      i == last_col ? "\n" : " ");
                }
            }
        }
        return output;
    }

    LIBASSERT_ATTR_COLD
    auto get_trace_window(const cpptrace::stacktrace& trace) {
        // Two boundaries: assert_detail and main
        // Both are found here, nothing is filtered currently at stack trace generation
        // (inlining and platform idiosyncrasies interfere)
        size_t start = 0;
        size_t end = trace.frames.size() - 1;
        for(size_t i = 0; i < trace.frames.size(); i++) {
            if(trace.frames[i].symbol.find("libassert::detail::") != std::string::npos) {
                start = i + 1;
            }
            if(trace.frames[i].symbol == "main" || trace.frames[i].symbol.find("main(") == 0) {
                end = i;
            }
        }
        return std::pair(start, end);
    }

    LIBASSERT_ATTR_COLD
    auto process_paths(const cpptrace::stacktrace& trace, size_t start, size_t end) {
        // raw full path -> components
        std::unordered_map<std::string, path_components> parsed_paths;
        // base file name -> path trie
        std::unordered_map<std::string, path_trie> tries;
        for(size_t i = start; i <= end; i++) {
            const auto& source_path = trace.frames[i].filename;
            if(!parsed_paths.count(source_path)) {
                auto parsed_path = parse_path(source_path);
                auto& file_name = parsed_path.back();
                parsed_paths.insert({source_path, parsed_path});
                if(tries.count(file_name) == 0) {
                    tries.insert({file_name, path_trie(file_name)});
                }
                tries.at(file_name).insert(parsed_path);
            }
        }
        // raw full path -> minified path
        std::unordered_map<std::string, std::string> files;
        size_t longest_file_width = 0;
        for(auto& [raw, parsed_path] : parsed_paths) {
            const std::string new_path = join(tries.at(parsed_path.back()).disambiguate(parsed_path), "/");
            internal_verify(files.insert({raw, new_path}).second);
            if(new_path.size() > longest_file_width) {
                longest_file_width = new_path.size();
            }
        }
        return std::pair(files, std::min(longest_file_width, size_t(50)));
    }

    LIBASSERT_ATTR_COLD [[nodiscard]]
    // TODO
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    std::string print_stacktrace(const cpptrace::raw_trace* raw_trace, int term_width, color_scheme scheme) {
        std::string stacktrace;
        if(raw_trace && !raw_trace->empty()) {
            auto trace = raw_trace->resolve();
            // [start, end] is an inclusive range
            auto [start, end] = get_trace_window(trace);
            // prettify signatures
            for(auto& frame : trace) {
                frame.symbol = prettify_type(frame.symbol);
            }
            // path preprocessing
            constexpr size_t max_file_length = 50;
            auto [files, longest_file_width] = process_paths(trace, start, end);
            // figure out column widths
            const auto max_line_number =
                std::max_element(
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
                    trace.begin() + start,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
                    trace.begin() + end + 1,
                    [](const cpptrace::stacktrace_frame& a, const cpptrace::stacktrace_frame& b) {
                        return a.line.value_or(0) < b.line.value_or(0);
                    }
                )->line;
            const size_t max_line_number_width = n_digits(max_line_number.value_or(0));
            const size_t max_frame_width = n_digits(end - start);
            // do the actual trace
            for(size_t i = start; i <= end; i++) {
                const auto& [address, line, col, source_path, signature, is_inline] = trace.frames[i];
                const std::string line_number = line.has_value() ? std::to_string(line.value()) : "?";
                // look for repeats, i.e. recursion we can fold
                size_t recursion_folded = 0;
                if(end - i >= 4) {
                    size_t j = 1;
                    for( ; i + j <= end; j++) {
                        if(trace.frames[i + j] != trace.frames[i] || trace.frames[i + j].symbol == "??") {
                            break;
                        }
                    }
                    if(j >= 4) {
                        recursion_folded = j - 2;
                    }
                }
                const size_t frame_number = i - start + 1;
                // pretty print with columns for wide terminals
                // split printing for small terminals
                if(term_width >= 50) {
                    auto sig = highlight_blocks(signature + "(", scheme); // hack for the highlighter
                    sig.pop_back();
                    const size_t left = 2 + max_frame_width;
                    // todo: is this looking right...?
                    const size_t middle = std::max(line_number.size(), max_line_number_width);
                    const size_t remaining_width = term_width - (left + middle + 3 /* spaces */);
                    LIBASSERT_PRIMITIVE_ASSERT(remaining_width >= 2);
                    const size_t file_width = std::min({longest_file_width, remaining_width / 2, max_file_length});
                    const size_t sig_width = remaining_width - file_width;
                    stacktrace += wrapped_print(
                        {
                            { 1,          {{"", "#"}} },
                            { left - 2,   highlight_blocks(std::to_string(frame_number), scheme), true },
                            { file_width, {{"", files.at(source_path)}} },
                            { middle,     highlight_blocks(line_number, scheme), true }, // intentionally not coloring "?"
                            { sig_width,  sig }
                        },
                        scheme
                    );
                } else {
                    auto sig = highlight(signature + "(", scheme); // hack for the highlighter
                    sig = sig.substr(0, sig.rfind('('));
                    stacktrace += stringf(
                        "#%s%2d%s %s\n      at %s:%s%s%s\n",
                        std::string(scheme.number).c_str(),
                        (int)frame_number,
                        std::string(scheme.reset).c_str(),
                        sig.c_str(),
                        files.at(source_path).c_str(),
                        std::string(scheme.number).c_str(),
                        line_number.c_str(),
                        std::string(scheme.reset).c_str() // yes this is excessive; intentionally coloring "?"
                    );
                }
                if(recursion_folded) {
                    i += recursion_folded;
                    const std::string s = stringf("| %d layers of recursion were folded |", recursion_folded);
                    (((stacktrace += scheme.accent) += stringf("|%*s|", int(s.size() - 2), "")) += scheme.reset) += '\n';
                    (((stacktrace += scheme.accent) += stringf("%s", s.c_str())) += scheme.reset) += '\n';
                    (((stacktrace += scheme.accent) += stringf("|%*s|", int(s.size() - 2), "")) += scheme.reset) += '\n';
                }
            }
        } else {
            stacktrace += "Error while generating stack trace.\n";
        }
        return stacktrace;
    }

    LIBASSERT_ATTR_COLD binary_diagnostics_descriptor::binary_diagnostics_descriptor() = default;
    LIBASSERT_ATTR_COLD binary_diagnostics_descriptor::binary_diagnostics_descriptor(
        std::string&& _lstring,
        std::string&& _rstring,
        std::string _a_str,
        std::string _b_str,
        bool _multiple_formats
    ):
        lstring(_lstring),
        rstring(_rstring),
        a_str(std::move(_a_str)),
        b_str(std::move(_b_str)),
        multiple_formats(_multiple_formats),
        present(true) {}
    LIBASSERT_ATTR_COLD binary_diagnostics_descriptor::~binary_diagnostics_descriptor() = default;
    LIBASSERT_ATTR_COLD
    binary_diagnostics_descriptor::binary_diagnostics_descriptor(binary_diagnostics_descriptor&&) noexcept = default;
    LIBASSERT_ATTR_COLD binary_diagnostics_descriptor&
    binary_diagnostics_descriptor::operator=(binary_diagnostics_descriptor&&) noexcept(LIBASSERT_GCC_ISNT_STUPID) = default;

    LIBASSERT_ATTR_COLD
    static std::string print_values(const std::vector<std::string>& vec, size_t lw, color_scheme scheme) {
        LIBASSERT_PRIMITIVE_ASSERT(!vec.empty());
        std::string values;
        if(vec.size() == 1) {
            values += stringf("%s\n", indent(highlight(vec[0], scheme), 8 + lw + 4, ' ', true).c_str());
        } else {
            // spacing here done carefully to achieve <expr> =  <a>  <b>  <c>, or similar
            // no indentation done here for multiple value printing
            values += " ";
            for(const auto& str : vec) {
                values += stringf("%s", highlight(str, scheme).c_str());
                if(&str != &*--vec.end()) {
                    values += "  ";
                }
            }
            values += "\n";
        }
        return values;
    }

    LIBASSERT_ATTR_COLD
    static std::vector<highlight_block> get_values(const std::vector<std::string>& vec, color_scheme scheme) {
        LIBASSERT_PRIMITIVE_ASSERT(!vec.empty());
        if(vec.size() == 1) {
            return highlight_blocks(vec[0], scheme);
        } else {
            std::vector<highlight_block> blocks;
            // spacing here done carefully to achieve <expr> =  <a>  <b>  <c>, or similar
            // no indentation done here for multiple value printing
            blocks.push_back({"", " "});
            for(const auto& str : vec) {
                auto h = highlight_blocks(str, scheme);
                blocks.insert(blocks.end(), h.begin(), h.end());
                if(&str != &*--vec.end()) {
                    blocks.push_back({"", "  "});
                }
            }
            return blocks;
        }
    }

    constexpr int min_term_width = 50;
    constexpr size_t arrow_width = " => "sv.size();
    constexpr size_t where_indent = 8;

    LIBASSERT_ATTR_COLD [[nodiscard]]
    std::string print_binary_diagnostics(
        binary_diagnostics_descriptor& diagnostics,
        size_t term_width,
        color_scheme scheme
    ) {
        auto& [ lstring, rstring, a_sstr, b_sstr, multiple_formats, _ ] = diagnostics;
        const std::string& a_str = a_sstr;
        const std::string& b_str = b_sstr;
        // TODO: Temporary hack while reworking
        std::vector<std::string> lstrings = { lstring };
        std::vector<std::string> rstrings = { rstring };
        LIBASSERT_PRIMITIVE_ASSERT(!lstrings.empty());
        LIBASSERT_PRIMITIVE_ASSERT(!rstrings.empty());
        // pad all columns where there is overlap
        // TODO: Use column printer instead of manual padding.
        for(size_t i = 0; i < std::min(lstrings.size(), rstrings.size()); i++) {
            // find which clause, left or right, we're padding (entry i)
            std::vector<std::string>& which = lstrings[i].length() < rstrings[i].length() ? lstrings : rstrings;
            const int difference = std::abs((int)lstrings[i].length() - (int)rstrings[i].length());
            if(i != which.size() - 1) { // last column excluded as padding is not necessary at the end of the line
                which[i].insert(which[i].end(), difference, ' ');
            }
        }
        // determine whether we actually gain anything from printing a where clause (e.g. exclude "1 => 1")
        const struct { bool left, right; } has_useful_where_clause = {
            multiple_formats || lstrings.size() > 1 || (a_str != lstrings[0] && trim_suffix(a_str) != lstrings[0]),
            multiple_formats || rstrings.size() > 1 || (b_str != rstrings[0] && trim_suffix(b_str) != rstrings[0])
        };
        // print where clause
        std::string where;
        if(has_useful_where_clause.left || has_useful_where_clause.right) {
            size_t lw = std::max(
                has_useful_where_clause.left  ? a_str.size() : 0,
                has_useful_where_clause.right ? b_str.size() : 0
            );
            // Limit lw to about half the screen. TODO: Re-evaluate what we want to do here.
            if(term_width > 0) {
                lw = std::min(lw, term_width / 2 - where_indent - arrow_width);
            }
            where += "    Where:\n";
            auto print_clause = [term_width, lw, &where, &scheme](
                const std::string& expr_str,
                const std::vector<std::string>& expr_strs
            ) {
                if(term_width >= min_term_width) {
                    where += wrapped_print(
                        {
                            { where_indent - 1, {{"", ""}} }, // 8 space indent, wrapper will add a space
                            { lw, highlight_blocks(expr_str, scheme) },
                            { 2, {{"", "=>"}} },
                            { term_width - lw - 8 /* indent */ - 4 /* arrow */, get_values(expr_strs, scheme) }
                        },
                        scheme
                    );
                } else {
                    where += stringf(
                        "        %s%*s => ",
                        highlight(expr_str, scheme).c_str(),
                        int(lw - expr_str.size()),
                        ""
                    );
                    where += print_values(expr_strs, lw, scheme);
                }
            };
            if(has_useful_where_clause.left) {
                print_clause(a_str, lstrings);
            }
            if(has_useful_where_clause.right) {
                print_clause(b_str, rstrings);
            }
        }
        return where;
    }

    LIBASSERT_ATTR_COLD [[nodiscard]]
    std::string print_extra_diagnostics(
        const decltype(extra_diagnostics::entries)& extra_diagnostics,
        size_t term_width,
        color_scheme scheme
    ) {
        std::string output = "    Extra diagnostics:\n";
        size_t lw = 0;
        for(const auto& entry : extra_diagnostics) {
            lw = std::max(lw, entry.first.size());
        }
        for(const auto& entry : extra_diagnostics) {
            if(term_width >= min_term_width) {
                output += wrapped_print(
                    {
                        { 7, {{"", ""}} }, // 8 space indent, wrapper will add a space
                        { lw, highlight_blocks(entry.first, scheme) },
                        { 2, {{"", "=>"}} },
                        { term_width - lw - 8 /* indent */ - 4 /* arrow */, highlight_blocks(entry.second, scheme) }
                    },
                    scheme
                );
            } else {
                output += stringf(
                    "        %s%*s => %s\n",
                    highlight(entry.first, scheme).c_str(),
                    int(lw - entry.first.length()),
                    "",
                    indent(
                        highlight(entry.second, scheme),
                        8 + lw + 4,
                        ' ',
                        true
                    ).c_str()
                );
            }
        }
        return output;
    }

    LIBASSERT_ATTR_COLD extra_diagnostics::extra_diagnostics() = default;
    LIBASSERT_ATTR_COLD extra_diagnostics::~extra_diagnostics() = default;
    LIBASSERT_ATTR_COLD extra_diagnostics::extra_diagnostics(extra_diagnostics&&) noexcept = default;

    LIBASSERT_ATTR_COLD
    const char* assert_type_name(assert_type t) {
        switch(t) {
            case assert_type::debug_assertion: return "Debug Assertion";
            case assert_type::assertion:       return "Assertion";
            case assert_type::assumption:      return "Assumption";
            case assert_type::verification:    return "Verification";
            default:
                LIBASSERT_PRIMITIVE_ASSERT(false);
                return "";
        }
    }

    LIBASSERT_ATTR_COLD
    size_t count_args_strings(const char* const* const arr) {
        size_t c = 0;
        for(size_t i = 0; *arr[i] != 0; i++) {
            c++;
        }
        return c + 1; // plus one, count the empty string
    }
}

namespace libassert {
    static std::atomic_bool output_colors = true;

    LIBASSERT_ATTR_COLD void set_color_output(bool enable) {
        output_colors = enable;
    }

    LIBASSERT_EXPORT color_scheme ansi_basic {
        BASIC_GREEN, /* string */
        BASIC_BLUE, /* escape */
        BASIC_PURPL, /* keyword */
        BASIC_ORANGE, /* named_literal */
        BASIC_CYAN, /* number */
        BASIC_PURPL, /* operator_token */
        BASIC_BLUE, /* call_identifier */
        BASIC_YELLOW, /* scope_resolution_identifier */
        BASIC_BLUE, /* identifier */
        BASIC_BLUE, /* accent */
        RESET
    };

    LIBASSERT_EXPORT color_scheme ansi_rgb {
        RGB_GREEN, /* string */
        RGB_BLUE, /* escape */
        RGB_PURPL, /* keyword */
        RGB_ORANGE, /* named_literal */
        RGB_CYAN, /* number */
        RGB_PURPL, /* operator_token */
        RGB_BLUE, /* call_identifier */
        RGB_YELLOW, /* scope_resolution_identifier */
        RGB_BLUE, /* identifier */
        RGB_BLUE, /* accent */
        RESET
    };

    LIBASSERT_EXPORT color_scheme blank_color_scheme;

    std::mutex color_scheme_mutex;
    color_scheme current_color_scheme = ansi_rgb;

    LIBASSERT_EXPORT void set_color_scheme(color_scheme scheme) {
        std::unique_lock lock(color_scheme_mutex);
        current_color_scheme = scheme;
    }

    LIBASSERT_EXPORT color_scheme get_color_scheme() {
        std::unique_lock lock(color_scheme_mutex);
        return current_color_scheme;
    }

    namespace detail {
        LIBASSERT_ATTR_COLD
        void libassert_default_failure_handler(
            assert_type type,
            const assertion_printer& printer
        ) {
            // TODO: Just throw instead of all of this?
            enable_virtual_terminal_processing_if_needed(); // for terminal colors on windows
            std::string message = printer(
                terminal_width(STDERR_FILENO),
                isatty(STDERR_FILENO) && output_colors ? get_color_scheme() : blank_color_scheme
            );
            std::cerr << message << std::endl;
            switch(type) {
                case assert_type::debug_assertion:
                case assert_type::assertion:
                    case assert_type::assumption: // switch-if-case, cursed!
                    (void)fflush(stderr);
                    std::abort();
                    // Breaking here as debug CRT allows aborts to be ignored, if someone wants to make a debug build of
                    // this library (on top of preventing fallthrough from nonfatal libassert)
                    break;
                case assert_type::verification:
                    throw verification_failure();
                    break;
                default:
                    LIBASSERT_PRIMITIVE_ASSERT(false);
            }
        }
    }

    static std::atomic failure_handler = detail::libassert_default_failure_handler;

    LIBASSERT_ATTR_COLD LIBASSERT_EXPORT
    void set_failure_handler(void (*handler)(assert_type, const assertion_printer&)) {
        failure_handler = handler;
    }

    namespace detail {
        LIBASSERT_ATTR_COLD LIBASSERT_EXPORT void fail(assert_type type, const assertion_printer& printer) {
            failure_handler.load()(type, printer);
        }
    }
}

namespace libassert {
    using namespace detail;

    const char* verification_failure::what() const noexcept {
        return "VERIFY() call failed";
    }

    LIBASSERT_ATTR_COLD assertion_printer::assertion_printer(
        const assert_static_parameters* _params,
        const extra_diagnostics& _processed_args,
        binary_diagnostics_descriptor& _binary_diagnostics,
        void* _raw_trace,
        size_t _sizeof_args
    ) :
        params(_params),
        processed_args(_processed_args),
        binary_diagnostics(_binary_diagnostics),
        raw_trace(_raw_trace),
        sizeof_args(_sizeof_args) {}

    LIBASSERT_ATTR_COLD assertion_printer::~assertion_printer() {
        auto* trace = static_cast<cpptrace::raw_trace*>(raw_trace);
        delete trace;
    }

    LIBASSERT_ATTR_COLD std::string assertion_printer::operator()(int width, color_scheme scheme) const {
        const auto& [ name, type, expr_str, location, args_strings ] = *params;
        const auto& [ message, extra_diagnostics, pretty_function ] = processed_args;
        std::string output;
        // generate header
        const auto function = prettify_type(pretty_function);
        if(!message.empty()) {
            output += stringf(
                "%s failed at %s:%d: %s: %s\n",
                assert_type_name(type),
                location.file,
                location.line,
                highlight(function, scheme).c_str(),
                message.c_str()
            );
        } else {
            output += stringf(
                "%s failed at %s:%d: %s:\n",
                assert_type_name(type),
                location.file,
                location.line,
                highlight(function, scheme).c_str()
            );
        }
        output += stringf(
            "    %s\n",
            highlight(
                stringf(
                    "%s(%s%s);",
                    name,
                    expr_str,
                    sizeof_args > 0 ? ", ..." : ""
                ),
                scheme
            ).c_str()
        );
        // generate binary diagnostics
        if(binary_diagnostics.present) {
            output += print_binary_diagnostics(binary_diagnostics, width, scheme);
        }
        // generate extra diagnostics
        if(!extra_diagnostics.empty()) {
            output += print_extra_diagnostics(extra_diagnostics, width, scheme);
        }
        // generate stack trace
        output += "\nStack trace:\n";
        output += print_stacktrace(static_cast<cpptrace::raw_trace*>(raw_trace), width, scheme);
        return output;
    }

    LIBASSERT_ATTR_COLD
    std::tuple<const char*, int, std::string, const char*> assertion_printer::get_assertion_info() const {
        const auto& location = params->location;
        auto function = prettify_type(processed_args.pretty_function);
        return {location.file, location.line, std::move(function), processed_args.message.c_str()};
    }
}

namespace libassert {
    LIBASSERT_ATTR_COLD [[nodiscard]] std::string stacktrace(int) {
        auto trace = cpptrace::generate_raw_trace();
        return ""; //print_stacktrace(&trace, width); // TODO FIXME
    }
}
