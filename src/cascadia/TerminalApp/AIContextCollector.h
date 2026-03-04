// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string>

namespace winrt::TerminalApp::implementation
{
    class AIContextCollector
    {
    public:
        // Collect context from the active terminal control.
        // Returns formatted terminal context string (last N lines, ANSI codes stripped).
        static std::wstring CollectFromTermControl(
            const winrt::Microsoft::Terminal::Control::TermControl& control,
            int maxLines = 50);

        // Strip ANSI escape sequences from text.
        static std::wstring StripAnsiCodes(const std::wstring& input);

        // Get current working directory from terminal (if available).
        static std::wstring GetWorkingDirectory(
            const winrt::Microsoft::Terminal::Control::TermControl& control);

    private:
        // Truncate to last N lines.
        static std::wstring _getLastNLines(const std::wstring& text, int n);
    };
}
