// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Security.Cryptography.EcDiffieHellman.Tests
{
    public interface IECDiffieHellmanProvider
    {
        ECDiffieHellman Create();
        ECDiffieHellman Create(int keySize);
#if NETCOREAPP
        ECDiffieHellman Create(ECCurve curve);
#endif
        bool IsCurveValid(Oid oid);
        bool ExplicitCurvesSupported { get; }
        bool ExplicitCurvesSupportFailOnUseOnly => PlatformDetection.IsAzureLinux;
        bool CanDeriveNewPublicKey { get; }
        bool SupportsRawDerivation { get; }
        bool SupportsSha3 { get; }
    }

    public static partial class ECDiffieHellmanFactory
    {
        public static ECDiffieHellman Create()
        {
            return s_provider.Create();
        }

        public static ECDiffieHellman Create(int keySize)
        {
            return s_provider.Create(keySize);
        }

#if NETCOREAPP
        public static ECDiffieHellman Create(ECCurve curve)
        {
            return s_provider.Create(curve);
        }
#endif

        public static bool IsCurveValid(Oid oid)
        {
            return s_provider.IsCurveValid(oid);
        }

        public static bool ExplicitCurvesSupported => s_provider.ExplicitCurvesSupported;

        public static bool CanDeriveNewPublicKey => s_provider.CanDeriveNewPublicKey;

        public static bool SupportsRawDerivation => s_provider.SupportsRawDerivation;

        public static bool SupportsSha3 => s_provider.SupportsSha3;

        public static bool ExplicitCurvesSupportFailOnUseOnly => s_provider.ExplicitCurvesSupportFailOnUseOnly;
    }
}
