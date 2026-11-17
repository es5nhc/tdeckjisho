#pragma once
// ============================================================================
//  Settings.hpp – a tiny persistent key=value store (one text file on SD)
// ============================================================================
//
//  File format (/config/settings.txt), human-editable on the PC:
//      # comment
//      font=16
//      ruby=1
//
//  Why a text file and not a binary struct dumped to disk? Because you can
//  open it in any editor, it survives adding/removing fields between firmware
//  versions, and it costs nothing measurable on a device that saves it a few
//  times per hour. Binary formats are for BIG data (see the dictionary).
//
//  std::map keeps keys sorted, so the saved file is stable/diffable.
//  Python: this class is essentially a dict with load()/save().
// ============================================================================

#include <map>
#include <string>

class Settings {
public:
    bool load(const std::string& path);
    bool save() const;

    std::string get(const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback) const;
    void set(const std::string& key, const std::string& value);
    void erase(const std::string& key);     // forget a key (no-op if absent)
    void setInt(const std::string& key, int value);

private:
    std::string _path;
    std::map<std::string, std::string> _values;
};
