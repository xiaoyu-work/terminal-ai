// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CopilotClient.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.Terminal.Settings.Model.h>

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
namespace WModel = winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    // ====================================================================
    // Helpers
    // ====================================================================

    static std::string wideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    static std::wstring utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
        return result;
    }

    static std::wstring jsonGetString(const JsonObject& obj, const winrt::hstring& key)
    {
        if (obj.HasKey(key))
        {
            const auto val = obj.GetNamedValue(key);
            if (val.ValueType() == JsonValueType::String)
            {
                return std::wstring{ val.GetString() };
            }
        }
        return {};
    }

    static int64_t jsonGetInt(const JsonObject& obj, const winrt::hstring& key, int64_t defaultVal = 0)
    {
        if (obj.HasKey(key))
        {
            const auto val = obj.GetNamedValue(key);
            if (val.ValueType() == JsonValueType::Number)
            {
                return static_cast<int64_t>(val.GetNumber());
            }
        }
        return defaultVal;
    }

    // ====================================================================
    // Constructor / Destructor
    // ====================================================================

    CopilotClient::CopilotClient() = default;

    CopilotClient::~CopilotClient()
    {
        Stop();
    }

    // ====================================================================
    // Process Lifecycle
    // ====================================================================

    IAsyncAction CopilotClient::StartAsync(
        const std::wstring& cliPath,
        const std::wstring& workingDirectory)
    {
        _workingDirectory = workingDirectory;

        // Create pipes for stdin/stdout
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE stdinRead = INVALID_HANDLE_VALUE;
        HANDLE stdinWrite = INVALID_HANDLE_VALUE;
        HANDLE stdoutRead = INVALID_HANDLE_VALUE;
        HANDLE stdoutWrite = INVALID_HANDLE_VALUE;

        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdinRead, &stdinWrite, &sa, 0));
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);

        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0));
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

        // Configure startup info with redirected handles
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdinRead;
        si.hStdOutput = stdoutWrite;
        si.hStdError = stdoutWrite; // merge stderr into stdout

        // Build command line: "copilot agent --headless"
        std::wstring cmdline = cliPath.empty() ? L"copilot" : cliPath;
        cmdline += L" agent --headless";
        // Mutable copy required by CreateProcessW
        std::vector<wchar_t> cmdlineBuf(cmdline.begin(), cmdline.end());
        cmdlineBuf.push_back(L'\0');

        const auto startDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();

        PROCESS_INFORMATION pi{};
        const auto created = CreateProcessW(
            nullptr,
            cmdlineBuf.data(),
            nullptr,
            nullptr,
            TRUE, // inherit handles
            CREATE_NO_WINDOW,
            nullptr,
            startDir,
            &si,
            &pi);

        // Close the child-side pipe ends (we don't need them)
        CloseHandle(stdinRead);
        CloseHandle(stdoutWrite);

        if (!created)
        {
            CloseHandle(stdinWrite);
            CloseHandle(stdoutRead);
            throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()),
                L"Failed to start copilot CLI process. Is 'copilot' installed and in PATH?");
        }

        _processHandle = pi.hProcess;
        _processId = pi.dwProcessId;
        CloseHandle(pi.hThread); // don't need thread handle

        _stdinWrite = stdinWrite;
        _stdoutRead = stdoutRead;
        _running = true;

        // Start reader thread
        _readerThread = std::thread([this]() { _readerLoop(); });

        // Ping to verify connectivity
        auto waiter = std::make_shared<AsyncWaiter>();
        {
            std::lock_guard lock(_pendingMutex);
            auto id = _sendRequest("ping", JsonObject{});
            _pendingRequests[id] = PendingRequest{
                [waiter](const JsonObject& result) { waiter->complete(result); },
                [waiter](int, const std::wstring& msg) { waiter->fail(msg); }
            };
        }

        co_await waiter->waitAsync();
    }

    void CopilotClient::Stop()
    {
        _running = false;

        _cleanup();

        if (_readerThread.joinable())
        {
            _readerThread.join();
        }

        // Cancel all pending requests
        std::lock_guard lock(_pendingMutex);
        for (auto& [id, req] : _pendingRequests)
        {
            if (req.onError)
            {
                req.onError(-1, L"Client stopped");
            }
        }
        _pendingRequests.clear();
    }

    bool CopilotClient::IsRunning() const noexcept
    {
        return _running.load();
    }

    void CopilotClient::_cleanup()
    {
        if (_stdinWrite != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_stdinWrite);
            _stdinWrite = INVALID_HANDLE_VALUE;
        }
        if (_stdoutRead != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_stdoutRead);
            _stdoutRead = INVALID_HANDLE_VALUE;
        }
        if (_processHandle != INVALID_HANDLE_VALUE)
        {
            TerminateProcess(_processHandle, 0);
            CloseHandle(_processHandle);
            _processHandle = INVALID_HANDLE_VALUE;
        }
    }

    // ====================================================================
    // Reader Thread (Content-Length framing, like LSP)
    // ====================================================================

    void CopilotClient::_readerLoop()
    {
        std::string buffer;
        char readBuf[4096];

        while (_running.load())
        {
            DWORD bytesRead = 0;
            const auto success = ReadFile(_stdoutRead, readBuf, sizeof(readBuf), &bytesRead, nullptr);
            if (!success || bytesRead == 0)
            {
                // Pipe closed or error - process likely exited
                _running = false;

                // Fire error event
                std::lock_guard lock(_callbackMutex);
                if (_currentCallback)
                {
                    CopilotEvent event;
                    event.type = CopilotEventType::SessionError;
                    event.errorMessage = L"Copilot CLI process exited unexpectedly";
                    _currentCallback(event);
                }
                break;
            }

            buffer.append(readBuf, bytesRead);

            // Parse Content-Length framed messages
            while (true)
            {
                // Look for "Content-Length: <N>\r\n\r\n"
                const auto headerEnd = buffer.find("\r\n\r\n");
                if (headerEnd == std::string::npos)
                    break;

                // Parse Content-Length from headers
                const auto headerStr = buffer.substr(0, headerEnd);
                size_t contentLength = 0;

                const auto clPos = headerStr.find("Content-Length: ");
                if (clPos != std::string::npos)
                {
                    const auto numStart = clPos + 16; // strlen("Content-Length: ")
                    const auto numEnd = headerStr.find("\r\n", numStart);
                    const auto numStr = headerStr.substr(numStart, numEnd != std::string::npos ? numEnd - numStart : std::string::npos);
                    try
                    {
                        contentLength = std::stoull(numStr);
                    }
                    catch (...)
                    {
                        // Bad header, skip
                        buffer.erase(0, headerEnd + 4);
                        continue;
                    }
                }

                if (contentLength == 0)
                {
                    buffer.erase(0, headerEnd + 4);
                    continue;
                }

                const auto bodyStart = headerEnd + 4;
                if (buffer.size() < bodyStart + contentLength)
                    break; // Need more data

                const auto jsonStr = buffer.substr(bodyStart, contentLength);
                buffer.erase(0, bodyStart + contentLength);

                try
                {
                    _processIncomingMessage(jsonStr);
                }
                catch (...)
                {
                    // Malformed JSON, skip
                }
            }
        }
    }

    // ====================================================================
    // JSON-RPC Message Processing
    // ====================================================================

    void CopilotClient::_processIncomingMessage(const std::string& json)
    {
        const auto wjson = utf8ToWide(json);
        JsonObject msg;
        if (!JsonObject::TryParse(winrt::hstring{ wjson }, msg))
            return;

        const auto hasId = msg.HasKey(L"id");
        const auto hasMethod = msg.HasKey(L"method");

        if (hasId && !hasMethod)
        {
            // This is a response to a request we sent
            _handleResponse(msg);
        }
        else if (hasId && hasMethod)
        {
            // This is a request from the server (permission, user input)
            _handleRequest(msg);
        }
        else if (hasMethod)
        {
            // This is a notification
            _handleNotification(msg);
        }
    }

    void CopilotClient::_handleResponse(const JsonObject& msg)
    {
        const auto id = static_cast<int64_t>(msg.GetNamedNumber(L"id"));

        std::lock_guard lock(_pendingMutex);
        auto it = _pendingRequests.find(id);
        if (it == _pendingRequests.end())
            return;

        auto req = std::move(it->second);
        _pendingRequests.erase(it);

        if (msg.HasKey(L"error"))
        {
            const auto err = msg.GetNamedObject(L"error");
            const auto code = static_cast<int>(err.GetNamedNumber(L"code", 0));
            const auto message = std::wstring{ err.GetNamedString(L"message", L"Unknown error") };
            if (req.onError)
                req.onError(code, message);
        }
        else if (msg.HasKey(L"result"))
        {
            if (req.onResult)
            {
                const auto resultVal = msg.GetNamedValue(L"result");
                if (resultVal.ValueType() == JsonValueType::Object)
                {
                    req.onResult(resultVal.GetObject());
                }
                else
                {
                    req.onResult(JsonObject{});
                }
            }
        }
    }

    void CopilotClient::_handleRequest(const JsonObject& msg)
    {
        // Server-initiated requests (permission, user input)
        const auto id = static_cast<int64_t>(msg.GetNamedNumber(L"id"));
        const auto method = std::wstring{ msg.GetNamedString(L"method") };

        JsonObject params;
        if (msg.HasKey(L"params"))
        {
            const auto paramsVal = msg.GetNamedValue(L"params");
            if (paramsVal.ValueType() == JsonValueType::Object)
            {
                params = paramsVal.GetObject();
            }
            else if (paramsVal.ValueType() == JsonValueType::Array)
            {
                // Some methods pass params as array with single object
                const auto arr = paramsVal.GetArray();
                if (arr.Size() > 0 && arr.GetAt(0).ValueType() == JsonValueType::Object)
                {
                    params = arr.GetAt(0).GetObject();
                }
            }
        }

        CopilotEvent event;
        event.rpcId = id;

        if (method == L"session.permissionRequest")
        {
            event.type = CopilotEventType::PermissionRequest;
            event.permissionKind = jsonGetString(params, L"kind");
            event.permissionDesc = jsonGetString(params, L"description");
            if (event.permissionDesc.empty())
            {
                event.permissionDesc = jsonGetString(params, L"command");
            }
            if (event.permissionDesc.empty())
            {
                event.permissionDesc = jsonGetString(params, L"path");
            }
        }
        else if (method == L"session.userInputRequest")
        {
            event.type = CopilotEventType::UserInputRequest;
            event.inputPrompt = jsonGetString(params, L"question");
            if (event.inputPrompt.empty())
            {
                event.inputPrompt = jsonGetString(params, L"prompt");
            }
        }
        else
        {
            // Unknown request - respond with error
            JsonObject errObj;
            errObj.Insert(L"code", JsonValue::CreateNumberValue(-32601));
            errObj.Insert(L"message", JsonValue::CreateStringValue(L"Method not found"));

            JsonObject response;
            response.Insert(L"jsonrpc", JsonValue::CreateStringValue(L"2.0"));
            response.Insert(L"id", JsonValue::CreateNumberValue(static_cast<double>(id)));
            response.Insert(L"error", errObj);

            _writeMessage(wideToUtf8(std::wstring{ response.Stringify() }));
            return;
        }

        std::lock_guard lock(_callbackMutex);
        if (_currentCallback)
        {
            _currentCallback(event);
        }
    }

    void CopilotClient::_handleNotification(const JsonObject& msg)
    {
        const auto method = std::wstring{ msg.GetNamedString(L"method") };

        JsonObject params;
        if (msg.HasKey(L"params"))
        {
            const auto paramsVal = msg.GetNamedValue(L"params");
            if (paramsVal.ValueType() == JsonValueType::Object)
            {
                params = paramsVal.GetObject();
            }
            else if (paramsVal.ValueType() == JsonValueType::Array)
            {
                const auto arr = paramsVal.GetArray();
                if (arr.Size() > 0 && arr.GetAt(0).ValueType() == JsonValueType::Object)
                {
                    params = arr.GetAt(0).GetObject();
                }
            }
        }

        _dispatchEvent(method, params);
    }

    // ====================================================================
    // Event Dispatch
    // ====================================================================

    void CopilotClient::_dispatchEvent(const std::wstring& method,
                                        const JsonObject& params)
    {
        CopilotEvent event;

        // Extract data from "data" sub-object if present
        JsonObject data;
        if (params.HasKey(L"data"))
        {
            const auto dataVal = params.GetNamedValue(L"data");
            if (dataVal.ValueType() == JsonValueType::Object)
            {
                data = dataVal.GetObject();
            }
        }
        else
        {
            data = params;
        }

        // Also check the "type" field inside params for event routing
        auto eventType = method;
        if (params.HasKey(L"type"))
        {
            eventType = std::wstring{ params.GetNamedString(L"type") };
        }

        if (eventType == L"assistant.message_delta")
        {
            event.type = CopilotEventType::MessageDelta;
            event.content = jsonGetString(data, L"deltaContent");
            if (event.content.empty())
            {
                event.content = jsonGetString(data, L"delta");
            }
        }
        else if (eventType == L"assistant.message")
        {
            event.type = CopilotEventType::Message;
            event.content = jsonGetString(data, L"content");
        }
        else if (eventType == L"assistant.reasoning_delta")
        {
            event.type = CopilotEventType::ReasoningDelta;
            event.content = jsonGetString(data, L"deltaContent");
            if (event.content.empty())
            {
                event.content = jsonGetString(data, L"delta");
            }
        }
        else if (eventType == L"tool.execution_start")
        {
            event.type = CopilotEventType::ToolExecutionStart;
            event.toolName = jsonGetString(data, L"toolName");
            event.toolCallId = jsonGetString(data, L"toolCallId");
            if (data.HasKey(L"arguments"))
            {
                const auto argsVal = data.GetNamedValue(L"arguments");
                if (argsVal.ValueType() == JsonValueType::Object)
                {
                    event.toolArgs = std::wstring{ argsVal.GetObject().Stringify() };
                }
                else if (argsVal.ValueType() == JsonValueType::String)
                {
                    event.toolArgs = std::wstring{ argsVal.GetString() };
                }
            }
        }
        else if (eventType == L"tool.execution_partial_result")
        {
            event.type = CopilotEventType::ToolPartialResult;
            event.toolCallId = jsonGetString(data, L"toolCallId");
            event.toolResult = jsonGetString(data, L"partialOutput");
        }
        else if (eventType == L"tool.execution_complete")
        {
            event.type = CopilotEventType::ToolComplete;
            event.toolCallId = jsonGetString(data, L"toolCallId");
            event.toolName = jsonGetString(data, L"toolName");
            event.toolSuccess = !data.HasKey(L"error") || data.GetNamedValue(L"error").ValueType() == JsonValueType::Null;
            if (data.HasKey(L"result"))
            {
                const auto resultVal = data.GetNamedValue(L"result");
                if (resultVal.ValueType() == JsonValueType::Object)
                {
                    event.toolResult = std::wstring{ resultVal.GetObject().Stringify() };
                }
                else if (resultVal.ValueType() == JsonValueType::String)
                {
                    event.toolResult = std::wstring{ resultVal.GetString() };
                }
            }
        }
        else if (eventType == L"session.idle")
        {
            event.type = CopilotEventType::SessionIdle;
        }
        else if (eventType == L"session.error")
        {
            event.type = CopilotEventType::SessionError;
            event.errorMessage = jsonGetString(data, L"message");
        }
        else if (eventType == L"assistant.usage")
        {
            event.type = CopilotEventType::Usage;
            event.inputTokens = static_cast<int32_t>(jsonGetInt(data, L"inputTokens"));
            event.outputTokens = static_cast<int32_t>(jsonGetInt(data, L"outputTokens"));
            event.model = jsonGetString(data, L"model");
        }
        else if (eventType == L"session.compaction_start")
        {
            event.type = CopilotEventType::CompactionStart;
        }
        else if (eventType == L"session.compaction_complete")
        {
            event.type = CopilotEventType::CompactionComplete;
        }
        else
        {
            // Unknown event type, ignore
            return;
        }

        std::lock_guard lock(_callbackMutex);
        if (_currentCallback)
        {
            _currentCallback(event);
        }
    }

    // ====================================================================
    // JSON-RPC Sending
    // ====================================================================

    int64_t CopilotClient::_sendRequest(const std::string& method,
                                         const JsonObject& params)
    {
        const auto id = _nextRequestId.fetch_add(1);

        JsonObject msg;
        msg.Insert(L"jsonrpc", JsonValue::CreateStringValue(L"2.0"));
        msg.Insert(L"id", JsonValue::CreateNumberValue(static_cast<double>(id)));
        msg.Insert(L"method", JsonValue::CreateStringValue(winrt::hstring{ utf8ToWide(method) }));

        // Wrap params in array (StreamJsonRpc convention)
        JsonArray paramsArray;
        paramsArray.Append(params);
        msg.Insert(L"params", paramsArray);

        _writeMessage(wideToUtf8(std::wstring{ msg.Stringify() }));
        return id;
    }

    void CopilotClient::_sendResponse(int64_t id, const JsonObject& result)
    {
        JsonObject msg;
        msg.Insert(L"jsonrpc", JsonValue::CreateStringValue(L"2.0"));
        msg.Insert(L"id", JsonValue::CreateNumberValue(static_cast<double>(id)));
        msg.Insert(L"result", result);

        _writeMessage(wideToUtf8(std::wstring{ msg.Stringify() }));
    }

    void CopilotClient::_writeMessage(const std::string& json)
    {
        if (_stdinWrite == INVALID_HANDLE_VALUE)
            return;

        // Content-Length framing
        const auto header = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n";
        const auto fullMsg = header + json;

        DWORD written = 0;
        WriteFile(_stdinWrite, fullMsg.data(), static_cast<DWORD>(fullMsg.size()), &written, nullptr);
    }

    // ====================================================================
    // Session Management
    // ====================================================================

    IAsyncAction CopilotClient::CreateSessionAsync(
        const std::wstring& model,
        const std::optional<CopilotProviderConfig>& providerConfig)
    {
        JsonObject params;
        if (!model.empty())
        {
            params.Insert(L"model", JsonValue::CreateStringValue(winrt::hstring{ model }));
        }
        params.Insert(L"streaming", JsonValue::CreateBooleanValue(true));

        if (providerConfig.has_value())
        {
            JsonObject provider;
            provider.Insert(L"type", JsonValue::CreateStringValue(winrt::hstring{ providerConfig->type }));
            if (!providerConfig->baseUrl.empty())
            {
                provider.Insert(L"baseUrl", JsonValue::CreateStringValue(winrt::hstring{ providerConfig->baseUrl }));
            }
            if (!providerConfig->apiKey.empty())
            {
                provider.Insert(L"apiKey", JsonValue::CreateStringValue(winrt::hstring{ providerConfig->apiKey }));
            }
            if (!providerConfig->wireApi.empty())
            {
                provider.Insert(L"wireApi", JsonValue::CreateStringValue(winrt::hstring{ providerConfig->wireApi }));
            }
            params.Insert(L"provider", provider);
        }

        if (!_workingDirectory.empty())
        {
            params.Insert(L"workingDirectory", JsonValue::CreateStringValue(winrt::hstring{ _workingDirectory }));
        }

        auto waiter = std::make_shared<AsyncWaiter>();
        {
            std::lock_guard lock(_pendingMutex);
            auto id = _sendRequest("session.create", params);
            _pendingRequests[id] = PendingRequest{
                [waiter, this](const JsonObject& result) {
                    _sessionId = jsonGetString(result, L"sessionId");
                    waiter->complete(result);
                },
                [waiter](int, const std::wstring& msg) { waiter->fail(msg); }
            };
        }

        co_await waiter->waitAsync();
    }

    IAsyncAction CopilotClient::ResumeSessionAsync(const std::wstring& sessionId)
    {
        JsonObject params;
        params.Insert(L"sessionId", JsonValue::CreateStringValue(winrt::hstring{ sessionId }));
        params.Insert(L"streaming", JsonValue::CreateBooleanValue(true));

        auto waiter = std::make_shared<AsyncWaiter>();
        {
            std::lock_guard lock(_pendingMutex);
            auto id = _sendRequest("session.resume", params);
            _pendingRequests[id] = PendingRequest{
                [waiter, this, sessionId](const JsonObject&) {
                    _sessionId = sessionId;
                    waiter->complete(JsonObject{});
                },
                [waiter](int, const std::wstring& msg) { waiter->fail(msg); }
            };
        }

        co_await waiter->waitAsync();
    }

    void CopilotClient::DestroySession()
    {
        if (_sessionId.empty())
            return;

        JsonObject params;
        params.Insert(L"sessionId", JsonValue::CreateStringValue(winrt::hstring{ _sessionId }));

        std::lock_guard lock(_pendingMutex);
        _sendRequest("session.destroy", params);
        // Fire and forget - don't wait for response
        _sessionId.clear();
    }

    std::wstring CopilotClient::SessionId() const
    {
        return _sessionId;
    }

    // ====================================================================
    // Messaging
    // ====================================================================

    IAsyncAction CopilotClient::SendMessageAsync(
        const std::wstring& message,
        CopilotEventCallback callback)
    {
        if (_sessionId.empty())
        {
            throw winrt::hresult_error(E_FAIL, L"No active session. Call CreateSessionAsync first.");
        }

        // Set the event callback
        {
            std::lock_guard lock(_callbackMutex);
            _currentCallback = callback;
        }

        JsonObject params;
        params.Insert(L"sessionId", JsonValue::CreateStringValue(winrt::hstring{ _sessionId }));
        params.Insert(L"prompt", JsonValue::CreateStringValue(winrt::hstring{ message }));

        auto waiter = std::make_shared<AsyncWaiter>();
        {
            std::lock_guard lock(_pendingMutex);
            auto id = _sendRequest("session.send", params);
            _pendingRequests[id] = PendingRequest{
                [waiter](const JsonObject& result) { waiter->complete(result); },
                [waiter](int, const std::wstring& msg) { waiter->fail(msg); }
            };
        }

        // Wait for the send to be acknowledged (returns messageId)
        co_await waiter->waitAsync();

        // Now wait for session.idle to indicate processing is complete
        auto idleWaiter = std::make_shared<AsyncWaiter>();
        {
            std::lock_guard lock(_callbackMutex);
            auto origCallback = _currentCallback;
            _currentCallback = [origCallback, idleWaiter](const CopilotEvent& event) {
                if (origCallback)
                    origCallback(event);

                if (event.type == CopilotEventType::SessionIdle ||
                    event.type == CopilotEventType::SessionError)
                {
                    idleWaiter->complete(JsonObject{});
                }
            };
        }

        co_await idleWaiter->waitAsync();

        // Clear callback
        {
            std::lock_guard lock(_callbackMutex);
            _currentCallback = nullptr;
        }
    }

    void CopilotClient::AbortCurrentRequest()
    {
        if (_sessionId.empty())
            return;

        JsonObject params;
        params.Insert(L"sessionId", JsonValue::CreateStringValue(winrt::hstring{ _sessionId }));

        std::lock_guard lock(_pendingMutex);
        _sendRequest("session.abort", params);
    }

    // ====================================================================
    // Permission / User Input Responses
    // ====================================================================

    void CopilotClient::RespondToPermission(int64_t rpcId, bool allowed)
    {
        JsonObject result;
        result.Insert(L"kind", JsonValue::CreateStringValue(
            allowed ? L"approved" : L"denied-interactively-by-user"));

        _sendResponse(rpcId, result);
    }

    void CopilotClient::RespondToUserInput(int64_t rpcId, const std::wstring& answer)
    {
        JsonObject result;
        result.Insert(L"answer", JsonValue::CreateStringValue(winrt::hstring{ answer }));
        result.Insert(L"wasFreeform", JsonValue::CreateBooleanValue(true));

        _sendResponse(rpcId, result);
    }

    // ====================================================================
    // Provider Config Mapping
    // ====================================================================

    CopilotProviderConfig CopilotClient::MapProviderConfig(
        WModel::AIProvider provider,
        const std::wstring& apiKey,
        const std::wstring& apiEndpoint,
        const std::wstring& /*model*/)
    {
        CopilotProviderConfig config;
        config.apiKey = apiKey;

        switch (provider)
        {
        case WModel::AIProvider::OpenAI:
            config.type = L"openai";
            config.baseUrl = apiEndpoint.empty() ? L"https://api.openai.com/v1" : apiEndpoint;
            break;
        case WModel::AIProvider::AzureOpenAI:
            config.type = L"azure";
            config.baseUrl = apiEndpoint;
            break;
        case WModel::AIProvider::Gemini:
            config.type = L"openai";
            config.baseUrl = apiEndpoint.empty() ? L"https://generativelanguage.googleapis.com/v1beta/openai" : apiEndpoint;
            break;
        case WModel::AIProvider::Ollama:
            config.type = L"openai";
            config.baseUrl = apiEndpoint.empty() ? L"http://localhost:11434/v1" : apiEndpoint;
            break;
        case WModel::AIProvider::DeepSeek:
            config.type = L"openai";
            config.baseUrl = apiEndpoint.empty() ? L"https://api.deepseek.com/v1" : apiEndpoint;
            break;
        case WModel::AIProvider::Anthropic:
            config.type = L"anthropic";
            config.baseUrl = apiEndpoint.empty() ? L"https://api.anthropic.com" : apiEndpoint;
            break;
        case WModel::AIProvider::Custom:
        default:
            config.type = L"openai";
            config.baseUrl = apiEndpoint;
            break;
        }

        return config;
    }
}
