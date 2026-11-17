#include "core/Settings.hpp"

#include "core/Str.hpp"
#include "hal/Hal.hpp"

namespace {

// SECURITY: the file format is "key=value", one per line. Whatever we store must come
// back as exactly the same key and value, and nothing else:
//   * a '\n' in a value (say, a URL typed or pasted with a line break) would start a new
//     line that the next load() reads as ANOTHER setting: "last_url=x\nlang=ja";
//   * a '=' in a key (a book called "a=b.txt") would split at the wrong place;
//   * a key starting with '#' would come back as a comment.
// So keys are escaped reversibly (%3D for '=', and so on), and control bytes in values
// become spaces. The map holds the ESCAPED keys, exactly as they are in the file.
std::string escapeKey(std::string_view key) {
    std::string out;
    for (char c : key) {
        const auto b = static_cast<uint8_t>(c);
        if (b <= 0x20 || c == '=' || c == '#' || c == '%') {
            static const char* const kHex = "0123456789ABCDEF";
            out += '%';
            out += kHex[b >> 4];
            out += kHex[b & 15];
        } else {
            out += c;
        }
    }
    return out;
}

std::string cleanValue(std::string_view value) {
    std::string out(value);
    for (char& c : out)
        if (static_cast<uint8_t>(c) < 0x20) c = ' ';
    return out;
}

}  // namespace

bool Settings::load(const std::string& path) {
    _path = path;
    _values.clear();
    std::string text;
    if (!hal::readFile(path, text, 64 * 1024)) return false;   // missing = defaults

    for (std::string_view line : str::split(text, '\n')) {
        line = str::trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        // operator[] creates the entry if it does not exist – like dict[k] = v
        _values[std::string(str::trim(line.substr(0, eq)))] =
            std::string(str::trim(line.substr(eq + 1)));
    }
    return true;
}

bool Settings::save() const {
    if (_path.empty()) return false;
    std::string text = "# TDeckJisho settings – edit freely, one key=value per line\n";
    // Structured binding `[k, v]`: unpacks the std::pair, like Python's
    // `for k, v in d.items():`
    for (const auto& [k, v] : _values) text += k + "=" + v + "\n";
    return hal::writeFile(_path, text);
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    const auto it = _values.find(escapeKey(key));   // find() never inserts
    return it == _values.end() ? fallback : it->second;
}

int Settings::getInt(const std::string& key, int fallback) const {
    const auto it = _values.find(escapeKey(key));
    return it == _values.end() ? fallback : static_cast<int>(str::toLong(it->second, fallback));
}

void Settings::set(const std::string& key, const std::string& value) {
    _values[escapeKey(key)] = cleanValue(value);
}

void Settings::setInt(const std::string& key, int value) { _values[escapeKey(key)] = std::to_string(value); }

void Settings::erase(const std::string& key) { _values.erase(escapeKey(key)); }
