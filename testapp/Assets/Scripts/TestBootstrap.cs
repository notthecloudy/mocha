using System;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.Scripting;

namespace MochaTestApp
{
    /// <summary>
    /// Writes startup evidence to both logcat and the app's persistent data directory.
    /// The file log survives logcat rotation and can be pulled with adb.
    /// </summary>
    [Preserve]
    public sealed class TestBootstrap : MonoBehaviour
    {
        private static string LogPath => Path.Combine(Application.persistentDataPath, "mocha-test.log");
        private static int s_lastHeartbeatFrame = -1;

        private static void Log(string message)
        {
            string line = $"{DateTime.UtcNow:O} {message}";
            try
            {
                File.AppendAllText(LogPath, line + Environment.NewLine, Encoding.UTF8);
            }
            catch (Exception e)
            {
                Debug.LogWarning("[MochaTest] file log failed: " + e.Message);
            }

            Debug.Log("[MochaTest] " + message);
        }

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void OnRuntimeInitialized()
        {
            Log($"runtime-init unity={Application.unityVersion} platform={Application.platform} " +
                $"persistent={Application.persistentDataPath}");
        }

        [Preserve]
        private void Awake()
        {
            DontDestroyOnLoad(gameObject);
            Log("awake");
        }

        [Preserve]
        private void Start()
        {
            Log("start");
        }

        [Preserve]
        private void Update()
        {
            int frame = Time.frameCount;
            if (s_lastHeartbeatFrame < 0 || frame - s_lastHeartbeatFrame >= 300)
            {
                s_lastHeartbeatFrame = frame;
                Log($"heartbeat frame={frame} counter={MochaTestTarget.Counter}");
            }
        }
    }
}
