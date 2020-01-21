// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Diagnostics;
using System.Runtime.InteropServices;

namespace System.Net.Security
{
#if DEBUG
    internal abstract class SafeDeleteContext : DebugSafeHandle
    {
#else
    internal abstract class SafeDeleteContext : SafeHandle
    {
#endif
        private SafeFreeCredentials _credential;

        //internal SafeHandle _handle;
        internal Interop.SspiCli.CredHandle _handle;

        protected SafeDeleteContext(SafeFreeCredentials credential = null)
            : base(IntPtr.Zero, true)
        {
            Debug.Assert((null != credential), "Invalid credential passed to SafeDeleteContext");

            // When a credential handle is first associated with the context we keep credential
            // ref count bumped up to ensure ordered finalization. The credential properties
            // are used in the SSL/NEGO data structures and should survive the lifetime of
            // the SSL/NEGO context
            bool ignore = false;
            _credential = credential;
            _credential.DangerousAddRef(ref ignore);
        }

        public override bool IsInvalid
        {
            get { return (null == _credential); }
        }

        protected override bool ReleaseHandle()
        {
            Debug.Assert((null != _credential), "Null credential in SafeDeleteContext");
            _credential.DangerousRelease();
            _credential = null;
            return true;
        }

        internal static unsafe int CompleteAuthToken(
            ref SafeDeleteSslContext refContext,
            in SecurityBuffer inSecBuffer)
        {
            throw new PlatformNotSupportedException();
        }

        internal static unsafe int AcceptSecurityContext(
            ref SafeFreeCredentials inCredentials,
            ref SafeDeleteSslContext refContext,
            Interop.SspiCli.ContextFlags inFlags,
            Interop.SspiCli.Endianness endianness,
            ReadOnlySpan<SecurityBuffer> inSecBuffers,
            ref SecurityBuffer outSecBuffer,
            ref Interop.SspiCli.ContextFlags outFlags)
        {
            throw new PlatformNotSupportedException();
        }

        internal static unsafe int InitializeSecurityContext(
            ref SafeFreeCredentials inCredentials,
            ref SafeDeleteSslContext refContext,
            string targetName,
            Interop.SspiCli.ContextFlags inFlags,
            Interop.SspiCli.Endianness endianness,
            ReadOnlySpan<SecurityBuffer> inSecBuffers,
            ref SecurityBuffer outSecBuffer,
            ref Interop.SspiCli.ContextFlags outFlags)
        {
            throw new PlatformNotSupportedException();
        }

        internal static unsafe int ApplyControlToken(
            ref SafeDeleteContext refContext,
            in SecurityBuffer inSecBuffer)
        {
            throw new PlatformNotSupportedException();
        }
    }
}
