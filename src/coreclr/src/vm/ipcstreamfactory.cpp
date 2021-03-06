// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "common.h"
#include "diagnosticsprotocol.h"
#include "ipcstreamfactory.h"

#ifdef FEATURE_PERFTRACING

CQuickArrayList<IpcStreamFactory::ConnectionState*> IpcStreamFactory::s_rgpConnectionStates = CQuickArrayList<IpcStreamFactory::ConnectionState*>();
Volatile<bool> IpcStreamFactory::s_isShutdown = false;

bool IpcStreamFactory::ClientConnectionState::GetIpcPollHandle(IpcStream::DiagnosticsIpc::IpcPollHandle *pIpcPollHandle, ErrorCallback callback)
{
    if (_pStream == nullptr)
    {
        // cache is empty, reconnect, e.g., there was a disconnect
        IpcStream *pConnection = _pIpc->Connect(callback);
        if (pConnection == nullptr)
        {
            if (callback != nullptr)
                callback("Failed to connect to client connection", -1);
            return false;
        }
        if (!DiagnosticsIpc::SendIpcAdvertise_V1(pConnection))
        {
            if (callback != nullptr)
                callback("Failed to send advertise message", -1);
            delete pConnection;
            return false;
        }

        _pStream = pConnection;
    }
    *pIpcPollHandle = { nullptr, _pStream, 0, this };
    return true;
}

IpcStream *IpcStreamFactory::ClientConnectionState::GetConnectedStream(ErrorCallback callback)
{
    IpcStream *pStream = _pStream;
    _pStream = nullptr;
    return pStream;
}

void IpcStreamFactory::ClientConnectionState::Reset(ErrorCallback callback)
{
    delete _pStream;
    _pStream = nullptr;
}

bool IpcStreamFactory::ServerConnectionState::GetIpcPollHandle(IpcStream::DiagnosticsIpc::IpcPollHandle *pIpcPollHandle, ErrorCallback callback)
{
    *pIpcPollHandle = { _pIpc, nullptr, 0, this };
    return true;
}

IpcStream *IpcStreamFactory::ServerConnectionState::GetConnectedStream(ErrorCallback callback)
{
    return _pIpc->Accept(callback);
}

// noop for server
void IpcStreamFactory::ServerConnectionState::Reset(ErrorCallback)
{
    return;
}

bool IpcStreamFactory::CreateServer(const char *const pIpcName, ErrorCallback callback)
{
    IpcStream::DiagnosticsIpc *pIpc = IpcStream::DiagnosticsIpc::Create(pIpcName, IpcStream::DiagnosticsIpc::ConnectionMode::SERVER, callback);
    if (pIpc != nullptr)
    {
        if (pIpc->Listen(callback))
        {
            s_rgpConnectionStates.Push(new ServerConnectionState(pIpc));
            return true;
        }
        else
        {
            delete pIpc;
            return false;
        }
    }
    else
    {
        return false;
    }
}

bool IpcStreamFactory::CreateClient(const char *const pIpcName, ErrorCallback callback)
{
    IpcStream::DiagnosticsIpc *pIpc = IpcStream::DiagnosticsIpc::Create(pIpcName, IpcStream::DiagnosticsIpc::ConnectionMode::CLIENT, callback);
    if (pIpc != nullptr)
    {
        s_rgpConnectionStates.Push(new ClientConnectionState(pIpc));
        return true;
    }
    else
    {
        return false;
    }
}

bool IpcStreamFactory::HasActiveConnections()
{
    return !s_isShutdown && s_rgpConnectionStates.Size() > 0;
}

void IpcStreamFactory::CloseConnections(ErrorCallback callback)
{
    for (uint32_t i = 0; i < (uint32_t)s_rgpConnectionStates.Size(); i++)
        s_rgpConnectionStates[i]->Close(callback);
}

void IpcStreamFactory::Shutdown(ErrorCallback callback)
{
    if (s_isShutdown)
        return;
    s_isShutdown = true;
    for (uint32_t i = 0; i < (uint32_t)s_rgpConnectionStates.Size(); i++)
        s_rgpConnectionStates[i]->Close(true, callback);
}

// helper function for getting timeout
int32_t IpcStreamFactory::GetNextTimeout(int32_t currentTimeoutMs)
{
    if (currentTimeoutMs == s_pollTimeoutInfinite)
    {
        return s_pollTimeoutMinMs;
    }
    else
    {
        return (currentTimeoutMs >= s_pollTimeoutMaxMs) ?
                    s_pollTimeoutMaxMs :
                    (int32_t)((float)currentTimeoutMs * s_pollTimeoutFalloffFactor);
    }
}

IpcStream *IpcStreamFactory::GetNextAvailableStream(ErrorCallback callback)
{
    IpcStream *pStream = nullptr;
    CQuickArrayList<IpcStream::DiagnosticsIpc::IpcPollHandle> rgIpcPollHandles;

    int32_t pollTimeoutMs = s_pollTimeoutInfinite;
    bool fConnectSuccess = true;
    uint32_t nPollAttempts = 0;

    while (pStream == nullptr)
    {
        fConnectSuccess = true;
        for (uint32_t i = 0; i < (uint32_t)s_rgpConnectionStates.Size(); i++)
        {
            IpcStream::DiagnosticsIpc::IpcPollHandle pollHandle = {};
            if (s_rgpConnectionStates[i]->GetIpcPollHandle(&pollHandle, callback))
            {
                rgIpcPollHandles.Push(pollHandle);
            }
            else
            {
                fConnectSuccess = false;
            }
        }

        pollTimeoutMs = fConnectSuccess ?
            s_pollTimeoutInfinite :
            GetNextTimeout(pollTimeoutMs);

        int32_t retval = IpcStream::DiagnosticsIpc::Poll(rgIpcPollHandles.Ptr(), (uint32_t)rgIpcPollHandles.Size(), pollTimeoutMs, callback);
        nPollAttempts++;
        STRESS_LOG2(LF_DIAGNOSTICS_PORT, LL_INFO10, "IpcStreamFactory::GetNextAvailableStream - Poll attempt: %d, timeout: %dms.\n", nPollAttempts, pollTimeoutMs);

        if (retval != 0)
        {
            for (uint32_t i = 0; i < (uint32_t)rgIpcPollHandles.Size(); i++)
            {
                switch ((IpcStream::DiagnosticsIpc::PollEvents)rgIpcPollHandles[i].revents)
                {
                    case IpcStream::DiagnosticsIpc::PollEvents::HANGUP:
                        ((ConnectionState*)(rgIpcPollHandles[i].pUserData))->Reset(callback);
                        STRESS_LOG1(LF_DIAGNOSTICS_PORT, LL_INFO10, "IpcStreamFactory::GetNextAvailableStream - Poll attempt: %d, connection hung up.\n", nPollAttempts);
                        pollTimeoutMs = s_pollTimeoutMinMs;
                        break;
                    case IpcStream::DiagnosticsIpc::PollEvents::SIGNALED:
                        if (pStream == nullptr) // only use first signaled stream; will get others on subsequent calls
                            pStream = ((ConnectionState*)(rgIpcPollHandles[i].pUserData))->GetConnectedStream(callback);
                        break;
                    case IpcStream::DiagnosticsIpc::PollEvents::ERR:
                        return nullptr;
                    default:
                        // TODO: Error handling
                        break;
                }
            }
        }

        // clear the view
        while (rgIpcPollHandles.Size() > 0)
            rgIpcPollHandles.Pop();
    }

    return pStream;
}

#endif // FEATURE_PERFTRACING