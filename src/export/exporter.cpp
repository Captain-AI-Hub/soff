#include "soff/export/exporter.hpp"

#include <sstream>

namespace soff {

std::string Exporter::describe(const ProgramSnapshot& snapshot) const
{
    std::ostringstream out;
    out << "snapshot input=" << snapshot.input_path
        << " arch=" << snapshot.architecture
        << " functions=" << snapshot.functions.size();
    return out.str();
}

} // namespace soff
