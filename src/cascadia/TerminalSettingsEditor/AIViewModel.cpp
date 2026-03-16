// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AIViewModel.h"
#include "AIViewModel.g.cpp"

#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Data::Json;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // GitHub Copilot OAuth constants (same as ATerm)
    static constexpr std::wstring_view ClientId{ L"Iv1.b507a08c87ecfe98" };
    static constexpr std::wstring_view Scopes{ L"read:user" };
    static constexpr std::wstring_view DeviceCodeUrl{ L"https://github.com/login/device/code" };
    static constexpr std::wstring_view TokenUrl{ L"https://github.com/login/oauth/access_token" };

    AIViewModel::AIViewModel(Model::AISettings aiSettings) :
        _aiSettings{ std::move(aiSettings) }
    {
    }

    int32_t AIViewModel::MaxContextBlocks() const { return _aiSettings ? _aiSettings.MaxContextBlocks() : 5; }
    void AIViewModel::MaxContextBlocks(int32_t value) { if (_aiSettings) { _aiSettings.MaxContextBlocks(value); _NotifyChanges(L"MaxContextBlocks"); } }

    winrt::hstring AIViewModel::CopilotCliPath() const { return _aiSettings ? _aiSettings.CopilotCliPath() : winrt::hstring{}; }
    void AIViewModel::CopilotCliPath(const winrt::hstring& value)
    {
        if (_aiSettings)
        {
            _aiSettings.CopilotCliPath(value);
            _NotifyChanges(L"CopilotCliPath", L"IsCopilotCliDetected");
        }
    }

    bool AIViewModel::IsCopilotCliDetected() const
    {
        return _aiSettings ? _aiSettings.IsCopilotCliDetected() : false;
    }

    bool AIViewModel::IsCopilotAuthLoggedIn() const
    {
        return _aiSettings ? _aiSettings.IsCopilotAuthLoggedIn() : false;
    }

    void AIViewModel::CopilotAuthLogin()
    {
        if (!_aiSettings || _isOAuthInProgress)
            return;
        _startDeviceCodeFlow();
    }

    void AIViewModel::OpenVerificationUri()
    {
        if (_verificationUri.empty())
            return;
        Windows::Foundation::Uri uri{ _verificationUri };
        Windows::System::Launcher::LaunchUriAsync(uri);
    }

    void AIViewModel::CopyUserCode()
    {
        if (_userCode.empty())
            return;
        Windows::ApplicationModel::DataTransfer::DataPackage dataPackage;
        dataPackage.SetText(_userCode);
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dataPackage);
    }

    void AIViewModel::CancelOAuth()
    {
        _pollCancelled = true;
        _isOAuthInProgress = false;
        _userCode = L"";
        _verificationUri = L"";
        _deviceCode.clear();
        _NotifyChanges(L"IsOAuthInProgress", L"UserCode", L"VerificationUri");
    }

    void AIViewModel::Disconnect()
    {
        if (_aiSettings)
        {
            _aiSettings.GithubToken(L"");
            _NotifyChanges(L"IsCopilotAuthLoggedIn");
        }
    }

    // Helper to build URL-encoded POST body as HttpStreamContent
    static IAsyncOperation<HttpStreamContent> makeFormContent(const std::wstring& body)
    {
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        Windows::Storage::Streams::DataWriter writer(stream);
        writer.UnicodeEncoding(Windows::Storage::Streams::UnicodeEncoding::Utf8);
        writer.WriteString(body);
        co_await writer.StoreAsync();
        co_await writer.FlushAsync();
        writer.DetachStream();
        stream.Seek(0);

        HttpStreamContent content(stream);
        content.Headers().ContentType(Headers::HttpMediaTypeHeaderValue::Parse(L"application/x-www-form-urlencoded"));
        co_return content;
    }

    winrt::fire_and_forget AIViewModel::_startDeviceCodeFlow()
    {
        // Capture UI thread context so we can resume on it for property updates
        winrt::apartment_context uiThread;
        auto weak = get_weak();

        _isOAuthInProgress = true;
        _oauthError = L"";
        _pollCancelled = false;
        _NotifyChanges(L"IsOAuthInProgress", L"OAuthError");

        try
        {
            // Step 1: Request device code (on background thread)
            co_await winrt::resume_background();

            HttpClient httpClient;
            httpClient.DefaultRequestHeaders().Accept().TryParseAdd(L"application/json");

            std::wstring requestBody = L"client_id=";
            requestBody += ClientId;
            requestBody += L"&scope=";
            requestBody += Scopes;

            auto content = co_await makeFormContent(requestBody);
            auto deviceCodeUri = Uri(DeviceCodeUrl);
            auto response = co_await httpClient.PostAsync(deviceCodeUri, content);
            auto responseStr = co_await response.Content().ReadAsStringAsync();

            if (!response.IsSuccessStatusCode())
            {
                co_await uiThread;
                auto strong = weak.get();
                if (!strong) co_return;

                strong->_oauthError = L"Device code request failed: " + responseStr;
                strong->_isOAuthInProgress = false;
                strong->_NotifyChanges(L"OAuthError", L"IsOAuthInProgress");
                co_return;
            }

            auto json = JsonObject::Parse(responseStr);
            auto deviceCode = std::wstring(json.GetNamedString(L"device_code"));
            auto userCode = json.GetNamedString(L"user_code");
            auto verificationUri = json.GetNamedString(L"verification_uri", L"");
            if (verificationUri.empty())
            {
                verificationUri = json.GetNamedString(L"verification_url", L"");
            }
            auto interval = static_cast<int>(json.GetNamedNumber(L"interval", 5));

            // Switch to UI thread to update properties
            co_await uiThread;
            {
                auto strong = weak.get();
                if (!strong) co_return;

                strong->_deviceCode = deviceCode;
                strong->_userCode = userCode;
                strong->_verificationUri = verificationUri;
                strong->_pollInterval = interval;
                strong->_NotifyChanges(L"UserCode", L"VerificationUri");
            }

            // Step 2: Poll for token
            std::wstring pollBody = L"client_id=";
            pollBody += ClientId;
            pollBody += L"&device_code=";
            pollBody += deviceCode;
            pollBody += L"&grant_type=urn:ietf:params:oauth:grant-type:device_code";

            auto tokenUri = Uri(TokenUrl);
            int pollIntervalMs = interval * 1000;

            while (true)
            {
                // Wait on background thread
                co_await winrt::resume_background();
                co_await winrt::resume_after(std::chrono::milliseconds(pollIntervalMs));

                {
                    auto strong = weak.get();
                    if (!strong || strong->_pollCancelled)
                        co_return;
                }

                auto pollContent = co_await makeFormContent(pollBody);
                auto pollResponse = co_await httpClient.PostAsync(tokenUri, pollContent);
                auto pollResponseStr = co_await pollResponse.Content().ReadAsStringAsync();
                auto pollJson = JsonObject::Parse(pollResponseStr);

                // Switch to UI thread for property updates
                co_await uiThread;

                // Check for access token
                if (pollJson.HasKey(L"access_token"))
                {
                    auto accessToken = pollJson.GetNamedString(L"access_token");

                    auto strong = weak.get();
                    if (!strong) co_return;

                    strong->_aiSettings.GithubToken(accessToken);
                    strong->_isOAuthInProgress = false;
                    strong->_userCode = L"";
                    strong->_verificationUri = L"";
                    strong->_deviceCode.clear();
                    strong->_NotifyChanges(L"IsCopilotAuthLoggedIn", L"IsOAuthInProgress", L"UserCode", L"VerificationUri");
                    co_return;
                }

                // Check error
                auto error = std::wstring(pollJson.GetNamedString(L"error", L""));
                if (error == L"authorization_pending")
                {
                    continue;
                }
                if (error == L"slow_down")
                {
                    pollIntervalMs += 5000;
                    continue;
                }

                // Terminal errors — update UI and stop
                {
                    auto strong = weak.get();
                    if (!strong) co_return;

                    if (error == L"expired_token")
                    {
                        strong->_oauthError = L"Device code expired. Please try again.";
                    }
                    else if (error == L"access_denied")
                    {
                        strong->_oauthError = L"Authorization denied by user.";
                    }
                    else
                    {
                        auto desc = pollJson.GetNamedString(L"error_description", L"");
                        strong->_oauthError = desc.empty() ? winrt::hstring{ L"Token polling failed: " + error } : desc;
                    }

                    strong->_isOAuthInProgress = false;
                    strong->_userCode = L"";
                    strong->_verificationUri = L"";
                    strong->_deviceCode.clear();
                    strong->_NotifyChanges(L"OAuthError", L"IsOAuthInProgress", L"UserCode", L"VerificationUri");
                    co_return;
                }
            }
        }
        catch (...)
        {
            // co_await is not allowed inside catch blocks, so save error and handle below
        }

        // If we reach here, an exception was thrown
        co_await uiThread;
        auto strong = weak.get();
        if (!strong) co_return;

        strong->_oauthError = L"OAuth flow failed. Check your network connection.";
        strong->_isOAuthInProgress = false;
        strong->_userCode = L"";
        strong->_verificationUri = L"";
        strong->_deviceCode.clear();
        strong->_NotifyChanges(L"OAuthError", L"IsOAuthInProgress", L"UserCode", L"VerificationUri");
    }
}
