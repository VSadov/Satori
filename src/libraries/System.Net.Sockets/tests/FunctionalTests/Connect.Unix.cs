// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Threading.Tasks;
using Xunit;

namespace System.Net.Sockets.Tests;

public class SocketBlockingModeTransitionTests
{
    [ActiveIssue("https://github.com/dotnet/runtime/issues/128141", TestPlatforms.Android)]
    [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
    public async Task ConnectAsync_WithBuffer_Succeeds()
    {
        using Socket listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        listener.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        listener.Listen(1);

        using Socket client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        using SocketAsyncEventArgs args = new SocketAsyncEventArgs();
        args.RemoteEndPoint = (IPEndPoint)listener.LocalEndPoint;
        args.SetBuffer(new byte[] { 1, 2, 3 }, 0, 3);

        TaskCompletionSource completed = new TaskCompletionSource();
        args.Completed += (_, _) => completed.SetResult();
        bool completedAsync = client.ConnectAsync(args);
        if (!completedAsync)
        {
            completed.SetResult();
        }

        await completed.Task.WaitAsync(System.TimeSpan.FromMilliseconds(TestSettings.PassingTestTimeout));
        Assert.Equal(SocketError.Success, args.SocketError);
        Assert.True(client.Blocking);

        int result = Interop.Sys.Fcntl.GetIsNonBlocking(client.SafeHandle, out bool isNonBlocking);
        Assert.NotEqual(-1, result);
        // Linux connect-with-buffer leaves the descriptor nonblocking only on the asynchronous path.
        Assert.Equal(completedAsync && !PlatformDetection.IsApplePlatform && !PlatformDetection.IsAndroid, isNonBlocking);
    }
}
