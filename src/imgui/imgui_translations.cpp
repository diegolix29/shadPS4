// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "core/emulator_settings.h"
#include "imgui_translations.h"

namespace ImguiTranslate {

const std::map<u32, std::map<std::string, std::string>> langMap = {
    {0, JapaneseMap},
    // {1, EnglishUsMap}, - not used
    {2, FrenchMap},
    {3, SpanishMap},
    {4, GermanMap},
    {5, ItalianMap},
    {6, DutchMap},
    {7, PortugesePtMap},
    {8, RussianMap},
    {9, KoreanMap},
    {10, ChineseTraditionalMap},
    {11, ChineseSimplifiedMap},
    {12, FinnishMap},
    {13, SwedishMap},
    {14, DanishMap},
    {15, NorwegianMap},
    {16, PolishMap},
    {17, PortugeseBrMap},
    // {18, "English (UK)"}, - not used
    {19, TurkishMap},
    {20, SpanishLatinAmericanMap},
    {21, ArabicMap},
    {22, FrenchCanadaMap},
    {23, CzechMap},
    {24, HungarianMap},
    {25, GreekMap},
    {26, RomanianMap},
    {27, ThaiMap},
    {28, VietnameseMap},
    {29, IndonesianMap},
    {30, UkranianMap},
};

std::string tr(std::string input) {
    // since we're coding in English
    if (Config::GetLanguage() == 1 || Config::GetLanguage() == 18)
        return input;

    const std::map<std::string, std::string> translationTable = langMap.at(Config::GetLanguage());

    if (!translationTable.contains(input)) {
        return input;
    }

    return translationTable.at(input);
}

} // namespace ImguiTranslate
