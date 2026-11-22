#include "cli/commands/prebuild.hpp"

#include "prebuild/prebuild.hpp"

namespace dc_determinism {
namespace cli {

int command_prebuild(const ParsedArgs& args) {
    PrebuildOptions options;
    options.image = args.text_or("image", "");
    options.emsdk_image = args.text_or("emsdk_image", kDefaultEmsdkImage);
    options.emsdk_image_explicit = args.has("emsdk_image");
    options.worker_only = args.flag("worker_only");
    options.wasm_worker_only = args.flag("wasm_worker_only");
    options.check = args.flag("check");
    options.image_id = args.text_or("image_id", "");
    options.build_user = args.text_or("build_user", "");
    // `--tool-fragment` may be repeated: the parser joins the values with "\n".
    std::string fragments = args.text_or("tool_fragment", "");
    std::string::size_type start = 0;
    while (start <= fragments.size() && !fragments.empty()) {
        std::string::size_type end = fragments.find('\n', start);
        if (end == std::string::npos) end = fragments.size();
        if (end > start) options.tool_fragments.push_back(fragments.substr(start, end - start));
        start = end + 1;
    }
    return run_prebuild(options);
}

}  // namespace cli
}  // namespace dc_determinism
