// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using Microsoft.Win32.SafeHandles;

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace System.Net.Security
{
#if DEBUG
    internal sealed class SafeFreeCertContext : DebugSafeHandle
    {
#else
    internal sealed class SafeFreeCertContext : SafeHandle
    {
#endif
        private readonly SafeX509Handle _certificate;

        public SafeFreeCertContext(SafeX509Handle certificate = null) : base(IntPtr.Zero, true)
        {
            // In certain scenarios (e.g. server querying for a client cert), the
            // input certificate may be invalid and this is OK
            if ((null != certificate) && !certificate.IsInvalid)
            {
                bool gotRef = false;
                certificate.DangerousAddRef(ref gotRef);
                Debug.Assert(gotRef, "Unexpected failure in AddRef of certificate");
                _certificate = certificate;
                handle = _certificate.DangerousGetHandle();
            }
        }

        // This must be ONLY called from this file.
        internal void Set(IntPtr value)
        {
            this.handle = value;
        }

        public override bool IsInvalid
        {
            get
            {
                return handle == IntPtr.Zero;
            }
        }

        protected override bool ReleaseHandle()
        {
            _certificate.DangerousRelease();
            _certificate.Dispose();
            return true;
        }
    }

}
