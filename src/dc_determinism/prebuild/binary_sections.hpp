// The executable sections of a PE (Windows) or ELF64 (Linux) file, for `dc_determinism prebuild --check`: when two builds of
// one artifact differ, the code sections tell whether only the metadata around the code (timestamps, embedded paths)
// differs or the generated code itself.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dc_determinism {

struct ExecutableSections {
    std::string format;                                          // "PE" or "ELF"
    std::vector<std::pair<std::string, std::string>> sections;  // name -> sha256 of the content, in file order
};

// PE: sections with the code or execute flag. ELF64 (little endian): SHF_EXECINSTR sections that occupy file space.
// nullopt for any other (or a damaged) file.
std::optional<ExecutableSections> executable_sections(const std::string& bytes);

struct CodeComparison {
    bool comparable = false;  // both files are PE or both are ELF64
    bool identical = false;   // the same sections with the same content
    std::vector<std::string> names;      // every executable section of either file
    std::vector<std::string> different;  // the names that are missing on one side or differ
};
CodeComparison compare_code_sections(const std::string& first, const std::string& second);

// "code sections identical (.text, .rdata)", "code sections differ (.text)" or "code sections not compared (...)".
std::string code_comparison_text(const CodeComparison& comparison);

}  // namespace dc_determinism
