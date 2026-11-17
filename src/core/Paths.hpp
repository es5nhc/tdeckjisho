#pragma once
// ============================================================================
//  Paths.hpp – where TDeckJisho keeps its files on the SD card
// ============================================================================
//
//  Everything lives under ONE folder, /tdeckjisho. The card is shared: Launcher
//  keeps its firmware images and settings there, other firmware (Meshtastic,
//  Bruce...) put their own folders at the root. A program that scatters
//  /books, /config, /jisho over the root will sooner or later collide with
//  someone else's /config. One folder per program also makes "remove
//  TDeckJisho" a single delete.
//
//      /tdeckjisho/
//          jisho/      the dictionary (entries.dat, kana.idx, kanji.idx, eng.idx, info.txt)
//          jmdict/     raw JMdict downloads / copies, input for "Update dictionary"
//          books/      your .txt / .md / .html files (+ their images)
//          config/     settings.txt, positions.txt, quiz.txt, wifi.txt, feeds.txt
//
//  Every path in the program comes from here, so renaming the folder is a
//  one-line change. (C++17 `inline` variables: one shared object even though
//  every .cpp that includes this header sees the definition.)
// ============================================================================

#include <string>

namespace paths {

inline const std::string kRoot     = "/tdeckjisho";
inline const std::string kDict     = kRoot + "/jisho";
inline const std::string kDictNew  = kRoot + "/jisho.new";   // a build in progress
inline const std::string kDictOld  = kRoot + "/jisho.old";   // the previous one, during the swap
inline const std::string kJmdict   = kRoot + "/jmdict";
inline const std::string kBooks    = kRoot + "/books";
inline const std::string kConfig   = kRoot + "/config";

inline const std::string kSettings  = kConfig + "/settings.txt";
inline const std::string kPositions = kConfig + "/positions.txt";
inline const std::string kQuiz      = kConfig + "/quiz.txt";   // answer streaks of the quiz
inline const std::string kWifi      = kConfig + "/wifi.txt";
inline const std::string kFeeds     = kConfig + "/feeds.txt";

}  // namespace paths
