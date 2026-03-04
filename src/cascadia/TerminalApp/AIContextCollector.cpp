// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AIContextCollector.h"
#include <regex>

namespace winrt::TerminalApp::implementation
{
    std::wstring AIContextCollector::CollectFromTermControl(
        const winrt::Microsoft::Terminal::Control::TermControl& control,
        int maxLines)
    {
        if (!control)
        {
            return L"";
        }

        try
        {
            // TermControl exposes ReadEntireBuffer() via ICoreState / TermControl.idl
            auto text = control.ReadEntireBuffer();

            auto stripped = StripAnsiCodes(std::wstring(text));
            auto lastLines = _getLastNLines(stripped, maxLines);

            return lastLines;
        }
        catch (...)
        {
            return L"[Unable to read terminal buffer]";
        }
    }

    std::wstring AIContextCollector::StripAnsiCodes(const std::wstring& input)
    {
        // Remove ANSI escape sequences: ESC[...m, ESC[...H, ESC[...J, etc.
        std::wregex ansiRegex(L"\\x1b\\[[0-9;]*[a-zA-Z]");
        return std::regex_replace(input, ansiRegex, L"");
    }

    std::wstring AIContextCollector::GetWorkingDirectory(
        const winrt::Microsoft::Terminal::Control::TermControl& control)
    {
        if (!control)
        {
            return L"";
        }

        try
        {
            // ICoreState provides WorkingDirectory on TermControl
            return std::wstring(control.WorkingDirectory());
        }
        catch (...)
        {
            return L"";
        }
    }

    std::wstring AIContextCollector::_getLastNLines(const std::wstring& text, int n)
    {
        std::vector<size_t> lineStarts;
        lineStarts.push_back(0);
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == L'\n')
            {
                lineStarts.push_back(i + 1);
            }
        }

        const auto totalLines = static_cast<int>(lineStarts.size());
        if (totalLines <= n)
        {
            return text;
        }

        const auto startPos = lineStarts[totalLines - n];
        return text.substr(startPos);
    }
}
