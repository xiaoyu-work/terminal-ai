// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <condition_variable>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <wil/resource.h>

// Forward declare the settings type
namespace winrt::Microsoft::Terminal::Settings::Model
{
    enum class AIProvider : int32_t;
}

namespace winrt::TerminalApp::implementation
{
    // ====================================================================
    // Event types matching the Copilot SDK streaming protocol
    // ====================================================================
    enum class CopilotEventType
    {
        MessageDelta,           // assistant.message_delta
        Message,                // assistant.message (complete)
        ReasoningDelta,         // assistant.reasoning_delta
        ToolExecutionStart,     // tool.execution_start
        ToolPartialResult,      // tool.execution_partial_result
        ToolComplete,           // tool.execution_complete
        SessionIdle,            // session.idle
        SessionError,           // session.error
        PermissionRequest,      // permission callback
        UserInputRequest,       // user input callback
        Usage,                  // assistant.usage
        CompactionStart,        // session.compaction_start
        CompactionComplete,     // session.compaction_complete
    };

    struct CopilotEvent
    {
        CopilotEventType type{};
        std::wstring content;           // For MessageDelta/Message/ReasoningDelta
        std::wstring toolName;          // For Tool* events
        std::wstring toolCallId;        // For Tool* events
        std::wstring toolArgs;          // For ToolExecutionStart (JSON string)
        std::wstring toolResult;        // For ToolComplete
        bool toolSuccess{ true };       // For ToolComplete
        std::wstring errorMessage;      // For SessionError
        std::wstring permissionKind;    // For PermissionRequest (shell/read/write/etc)
        std::wstring permissionDesc;    // For PermissionRequest
        int64_t rpcId{ 0 };            // For PermissionRequest/UserInputRequest (RPC request id to respond to)
        std::wstring inputPrompt;       // For UserInputRequest
        int32_t inputTokens{ 0 };       // For Usage
        int32_t outputTokens{ 0 };      // For Usage
        std::wstring model;             // For Usage
    };

    using CopilotEventCallback = std::function<void(const CopilotEvent&)>;

    // Provider configuration for BYOK
    struct CopilotProviderConfig
    {
        std::wstring type;      // "openai", "azure", "anthropic"
        std::wstring baseUrl;
        std::wstring apiKey;
        std::wstring wireApi;   // "completions" or "responses"
    };

    // ====================================================================
    // CopilotClient - JSON-RPC client wrapping copilot CLI process
    // ====================================================================
    class CopilotClient
    {
    public:
        CopilotClient();
        ~CopilotClient();

        // Lifecycle
        winrt::Windows::Foundation::IAsyncAction StartAsync(
            const std::wstring& cliPath,
            const std::wstring& workingDirectory);
        void Stop();
        bool IsRunning() const noexcept;

        // Session management
        winrt::Windows::Foundation::IAsyncAction CreateSessionAsync(
            const std::wstring& model,
            const std::optional<CopilotProviderConfig>& providerConfig);
        winrt::Windows::Foundation::IAsyncAction ResumeSessionAsync(
            const std::wstring& sessionId);
        void DestroySession();

        // Messaging
        winrt::Windows::Foundation::IAsyncAction SendMessageAsync(
            const std::wstring& message,
            CopilotEventCallback callback);
        void AbortCurrentRequest();

        // Permission/input responses (called from UI thread)
        void RespondToPermission(int64_t rpcId, bool allowed);
        void RespondToUserInput(int64_t rpcId, const std::wstring& answer);

        // Session info
        std::wstring SessionId() const;

        // Map AIProvider enum to SDK provider config
        static CopilotProviderConfig MapProviderConfig(
            winrt::Microsoft::Terminal::Settings::Model::AIProvider provider,
            const std::wstring& apiKey,
            const std::wstring& apiEndpoint,
            const std::wstring& model);

    private:
        // Process management
        HANDLE _processHandle{ INVALID_HANDLE_VALUE };
        HANDLE _stdinWrite{ INVALID_HANDLE_VALUE };
        HANDLE _stdoutRead{ INVALID_HANDLE_VALUE };
        DWORD _processId{ 0 };
        std::atomic<bool> _running{ false };

        // Reader thread
        std::thread _readerThread;
        void _readerLoop();

        // JSON-RPC state
        std::atomic<int64_t> _nextRequestId{ 1 };
        std::mutex _pendingMutex;

        // Pending RPC requests awaiting response
        struct PendingRequest
        {
            std::function<void(const winrt::Windows::Data::Json::JsonObject&)> onResult;
            std::function<void(int code, const std::wstring& message)> onError;
        };
        std::unordered_map<int64_t, PendingRequest> _pendingRequests;

        // Current event callback for streaming notifications
        CopilotEventCallback _currentCallback;
        std::mutex _callbackMutex;

        // Session state
        std::wstring _sessionId;
        std::wstring _workingDirectory;

        // JSON-RPC helpers
        int64_t _sendRequest(const std::string& method,
                             const winrt::Windows::Data::Json::JsonObject& params);
        void _sendResponse(int64_t id, const winrt::Windows::Data::Json::JsonObject& result);
        void _writeMessage(const std::string& json);

        // Message parsing (Content-Length framing)
        void _processIncomingMessage(const std::string& json);
        void _handleResponse(const winrt::Windows::Data::Json::JsonObject& msg);
        void _handleRequest(const winrt::Windows::Data::Json::JsonObject& msg);
        void _handleNotification(const winrt::Windows::Data::Json::JsonObject& msg);

        // Dispatch SDK notification to CopilotEvent
        void _dispatchEvent(const std::wstring& method,
                            const winrt::Windows::Data::Json::JsonObject& params);

        // Synchronous wait helpers for coroutines
        struct AsyncWaiter
        {
            std::mutex mtx;
            std::condition_variable cv;
            bool completed{ false };
            std::optional<winrt::Windows::Data::Json::JsonObject> result;
            std::optional<std::wstring> error;

            void complete(const winrt::Windows::Data::Json::JsonObject& r)
            {
                std::lock_guard lock(mtx);
                result = r;
                completed = true;
                cv.notify_one();
            }

            void fail(const std::wstring& msg)
            {
                std::lock_guard lock(mtx);
                error = msg;
                completed = true;
                cv.notify_one();
            }

            winrt::Windows::Foundation::IAsyncAction waitAsync()
            {
                co_await winrt::resume_background();
                std::unique_lock lock(mtx);
                cv.wait(lock, [this] { return completed; });
                if (error.has_value())
                {
                    throw winrt::hresult_error(E_FAIL, winrt::hstring{ error.value() });
                }
            }
        };

        // Cleanup
        void _cleanup();
    };
}
