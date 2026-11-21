using System;
using UnityEngine;
using UnityEngine.Scripting;

namespace MochaTestApp
{
    /// <summary>
    /// Stable surface used by the native Mocha integration tests.
    /// Every member is [Preserve]d so IL2CPP managed code stripping cannot remove it.
    /// </summary>
    [Preserve]
    public static class MochaTestTarget
    {
        public static int Counter;

        [Preserve]
        public static int Add(int a, int b) => a + b;

        [Preserve]
        public static string Echo(string value) => "echo:" + value;

        [Preserve]
        public static string Info() =>
            $"counter={Counter};unity={Application.unityVersion};persistent={Application.persistentDataPath}";

        [Preserve]
        public static void Increment() => Counter++;

        [Preserve]
        public static string ThrowIfRequested(bool doThrow)
        {
            if (doThrow)
            {
                throw new InvalidOperationException("intentional mocha test exception");
            }

            return "ok";
        }
    }
}
