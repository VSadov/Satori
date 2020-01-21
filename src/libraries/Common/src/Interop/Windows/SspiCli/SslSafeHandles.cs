// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Authentication.ExtendedProtection;
using Microsoft.Win32.SafeHandles;

namespace System.Net.Security
{
    //
    // Used when working with SSPI APIs, like SafeSspiAuthDataHandle(). Holds the pointer to the auth data blob.
    //
#if DEBUG
    internal sealed class SafeSspiAuthDataHandle : DebugSafeHandle
    {
#else
    internal sealed class SafeSspiAuthDataHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
#endif
        public SafeSspiAuthDataHandle() : base(true)
        {
        }

        protected override bool ReleaseHandle()
        {
            return Interop.SspiCli.SspiFreeAuthIdentity(handle) == Interop.SECURITY_STATUS.OK;
        }
    }


    //
    //  A set of Safe Handles that depend on native FreeContextBuffer finalizer.
    //
#if DEBUG
    internal abstract class SafeFreeContextBuffer : DebugSafeHandle
    {
#else
    internal abstract class SafeFreeContextBuffer : SafeHandleZeroOrMinusOneIsInvalid
    {
#endif
        protected SafeFreeContextBuffer() : base(true) { }

        // This must be ONLY called from this file.
        internal void Set(IntPtr value)
        {
            this.handle = value;
        }

        internal static int EnumeratePackages(out int pkgnum, out SafeFreeContextBuffer pkgArray)
        {
            int res = -1;
            SafeFreeContextBuffer_SECURITY pkgArray_SECURITY = null;
            res = Interop.SspiCli.EnumerateSecurityPackagesW(out pkgnum, out pkgArray_SECURITY);
            pkgArray = pkgArray_SECURITY;

            if (res != 0)
            {
                pkgArray?.SetHandleAsInvalid();
            }

            return res;
        }

        internal static SafeFreeContextBuffer CreateEmptyHandle()
        {
            return new SafeFreeContextBuffer_SECURITY();
        }

        //
        // After PInvoke call the method will fix the refHandle.handle with the returned value.
        // The caller is responsible for creating a correct SafeHandle template or null can be passed if no handle is returned.
        //
        // This method switches between three non-interruptible helper methods.  (This method can't be both non-interruptible and
        // reference imports from all three DLLs - doing so would cause all three DLLs to try to be bound to.)
        //
        public static unsafe int QueryContextAttributes(SafeDeleteContext phContext, Interop.SspiCli.ContextAttribute contextAttribute, byte* buffer, SafeHandle refHandle)
        {
            int status = (int)Interop.SECURITY_STATUS.InvalidHandle;

            try
            {
                bool ignore = false;
                phContext.DangerousAddRef(ref ignore);
                status = Interop.SspiCli.QueryContextAttributesW(ref phContext._handle, contextAttribute, buffer);
            }
            finally
            {
                phContext.DangerousRelease();
            }

            if (status == 0 && refHandle != null)
            {
                if (refHandle is SafeFreeContextBuffer)
                {
                    ((SafeFreeContextBuffer)refHandle).Set(*(IntPtr*)buffer);
                }
                else
                {
                    ((SafeFreeCertContext)refHandle).Set(*(IntPtr*)buffer);
                }
            }

            if (status != 0)
            {
                refHandle?.SetHandleAsInvalid();
            }

            return status;
        }

        public static int SetContextAttributes(
            SafeDeleteContext phContext,
            Interop.SspiCli.ContextAttribute contextAttribute, byte[] buffer)
        {
            try
            {
                bool ignore = false;
                phContext.DangerousAddRef(ref ignore);
                return Interop.SspiCli.SetContextAttributesW(ref phContext._handle, contextAttribute, buffer, buffer.Length);
            }
            finally
            {
                phContext.DangerousRelease();
            }
        }
    }

   // Based on SafeFreeContextBuffer.
    internal abstract class SafeFreeContextBufferChannelBinding : ChannelBinding
    {
        private int _size;

        public override int Size
        {
            get { return _size; }
        }

        public override bool IsInvalid
        {
            get { return handle == new IntPtr(0) || handle == new IntPtr(-1); }
        }

        internal unsafe void Set(IntPtr value)
        {
            this.handle = value;
        }

        internal static SafeFreeContextBufferChannelBinding CreateEmptyHandle()
        {
            return new SafeFreeContextBufferChannelBinding_SECURITY();
        }

        public static unsafe int QueryContextChannelBinding(SafeDeleteContext phContext, Interop.SspiCli.ContextAttribute contextAttribute, SecPkgContext_Bindings* buffer, SafeFreeContextBufferChannelBinding refHandle)
        {
            int status = (int)Interop.SECURITY_STATUS.InvalidHandle;

            // SCHANNEL only supports SECPKG_ATTR_ENDPOINT_BINDINGS and SECPKG_ATTR_UNIQUE_BINDINGS which
            // map to our enum ChannelBindingKind.Endpoint and ChannelBindingKind.Unique.
            if (contextAttribute != Interop.SspiCli.ContextAttribute.SECPKG_ATTR_ENDPOINT_BINDINGS &&
                contextAttribute != Interop.SspiCli.ContextAttribute.SECPKG_ATTR_UNIQUE_BINDINGS)
            {
                return status;
            }

            try
            {
                bool ignore = false;
                phContext.DangerousAddRef(ref ignore);
                status = Interop.SspiCli.QueryContextAttributesW(ref phContext._handle, contextAttribute, buffer);
            }
            finally
            {
                phContext.DangerousRelease();
            }

            if (status == 0 && refHandle != null)
            {
                refHandle.Set((*buffer).Bindings);
                refHandle._size = (*buffer).BindingsLength;
            }

            if (status != 0)
            {
                refHandle?.SetHandleAsInvalid();
            }

            return status;
        }

        public override string ToString()
        {
            if (IsInvalid)
            {
                return null;
            }

            var bytes = new byte[_size];
            Marshal.Copy(handle, bytes, 0, bytes.Length);
            return BitConverter.ToString(bytes).Replace('-', ' ');
        }
    }

    internal sealed class SafeFreeContextBuffer_SECURITY : SafeFreeContextBuffer
    {
        internal SafeFreeContextBuffer_SECURITY() : base() { }

        protected override bool ReleaseHandle()
        {
            return Interop.SspiCli.FreeContextBuffer(handle) == 0;
        }
    }
}
