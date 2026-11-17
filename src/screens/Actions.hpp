#pragma once
// ============================================================================
//  Actions.hpp – "open X" functions: the app's navigation in one place
// ============================================================================
//
//  Screens never construct each other directly; they call these. Benefits:
//    * one place to read to understand the whole app flow
//    * screens don't need to #include each other (fewer dependencies,
//      faster builds, no circular includes)
//  Think of it as the "controller" or the URL router of a web app.
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

#include "dict/Dictionary.hpp"
#include "text/RichText.hpp"
#include "ui/App.hpp"

namespace actions {

std::unique_ptr<Screen> home(App& app);

void openDictionary(App& app);
void openQuiz(App& app);         // 20 multiple-choice questions from the dictionary
void openEntry(App& app, uint32_t offset);
void openLibrary(App& app);
void openFile(App& app, const std::string& path);
void openFeeds(App& app);
void openFeed(App& app, const std::string& name, const std::string& url);
void openWebPrompt(App& app);
void openUrl(App& app, const std::string& url);
void openWifi(App& app);
void openUpdateDict(App& app);   // download JMdict / build on the device
void openLanguage(App& app);     // choose the user-interface language
void openAbout(App& app);
void openTouchTest(App& app);   // Home → p: check the touchscreen mapping

// Exposed for testing / reuse: dictionary entry -> readable document.
rt::Doc entryToDoc(const DictEntry& e);

}  // namespace actions
