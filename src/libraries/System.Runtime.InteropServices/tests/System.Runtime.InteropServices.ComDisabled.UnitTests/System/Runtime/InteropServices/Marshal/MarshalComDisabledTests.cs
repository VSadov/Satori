// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using Xunit;

namespace System.Runtime.InteropServices.Tests
{
    [PlatformSpecific(TestPlatforms.Windows)]
    [SkipOnMono("COM Interop not supported on Mono")]
    public partial class MarshalComDisabledTests
    {
        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetTypeFromCLSID_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetTypeFromCLSID(Guid.Empty));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void CreateAggregatedObject_ThrowsNotSupportedException()
        {
            object value = new object();
            Assert.Throws<NotSupportedException>(() => Marshal.CreateAggregatedObject(IntPtr.Zero, value));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void CreateAggregatedObject_T_ThrowsNotSupportedException()
        {
            object value = new object();
            Assert.Throws<NotSupportedException>(() => Marshal.CreateAggregatedObject<object>(IntPtr.Zero, value));
        }


        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void ReleaseComObject_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.ReleaseComObject(new object()));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void FinalReleaseComObject_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.FinalReleaseComObject(new object()));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetComObjectData_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetComObjectData("key", "value"));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void SetComObjectData_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.SetComObjectData(new object(), "key", "value"));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void CreateWrapperOfType_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.CreateWrapperOfType(new object(), typeof(object)));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void CreateWrapperOfType_T_TWrapper_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.CreateWrapperOfType<object, object>(new object()));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/72911", typeof(PlatformDetection), nameof(PlatformDetection.IsNativeAot))]
        public void GetNativeVariantForObject_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetNativeVariantForObject(99, IntPtr.Zero));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/72911", typeof(PlatformDetection), nameof(PlatformDetection.IsNativeAot))]
        public void GetNativeVariantForObject_T_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetNativeVariantForObject<double>(99, IntPtr.Zero));
        }

        public struct NativeVariant{}

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/72911", typeof(PlatformDetection), nameof(PlatformDetection.IsNativeAot))]
        public void GetObjectForNativeVariant_ThrowsNotSupportedException()
        {
            NativeVariant variant = new NativeVariant();
            IntPtr ptr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeVariant>());
            try
            {
                Marshal.StructureToPtr(variant, ptr, fDeleteOld: false);
                Assert.Throws<NotSupportedException>(() => Marshal.GetObjectForNativeVariant(ptr));
            }
            finally
            {
                Marshal.DestroyStructure<NativeVariant>(ptr);
                Marshal.FreeHGlobal(ptr);
            }
        }

        public struct NativeVariant_T{}

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/72911", typeof(PlatformDetection), nameof(PlatformDetection.IsNativeAot))]
        public void GetObjectForNativeVariant_T_ThrowsNotSupportedException()
        {
            NativeVariant_T variant = new NativeVariant_T();
            IntPtr ptr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeVariant_T>());
            try
            {
                Marshal.StructureToPtr(variant, ptr, fDeleteOld: false);
                Assert.Throws<NotSupportedException>(() => Marshal.GetObjectForNativeVariant<NativeVariant_T>(ptr));
            }
            finally
            {
                Marshal.DestroyStructure<NativeVariant_T>(ptr);
                Marshal.FreeHGlobal(ptr);
            }
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetObjectsForNativeVariants_ThrowsNotSupportedException()
        {
            IntPtr ptr = Marshal.AllocHGlobal(2 * Marshal.SizeOf<NativeVariant>());
            try
            {
                Assert.Throws<NotSupportedException>(() => Marshal.GetObjectsForNativeVariants(ptr, 2));
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetObjectsForNativeVariants_T_ThrowsNotSupportedException()
        {
            IntPtr ptr = Marshal.AllocHGlobal(2 * Marshal.SizeOf<NativeVariant_T>());
            try
            {
                Assert.Throws<NotSupportedException>(() => Marshal.GetObjectsForNativeVariants<sbyte>(ptr, 2));
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void BindToMoniker_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.BindToMoniker("test"));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetIUnknownForObject_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetIUnknownForObject(new object()));
        }

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetIDispatchForObject_ThrowsNotSupportedException()
        {
            Assert.Throws<NotSupportedException>(() => Marshal.GetIDispatchForObject(new object()));
        }

        public struct StructForIUnknown{}

        [Fact]
        [ActiveIssue("Satori: noisy test fails in baseline too")]
        public void GetObjectForIUnknown_ThrowsNotSupportedException()
        {
            StructForIUnknown test = new StructForIUnknown();
            IntPtr ptr = Marshal.AllocHGlobal(Marshal.SizeOf<StructForIUnknown>());
            try
            {
                Marshal.StructureToPtr(test, ptr, fDeleteOld: false);
                Assert.Throws<NotSupportedException>(() => Marshal.GetObjectForIUnknown(ptr));
            }
            finally
            {
                Marshal.DestroyStructure<StructForIUnknown>(ptr);
                Marshal.FreeHGlobal(ptr);
            }
        }
    }
}
