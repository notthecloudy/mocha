using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace MochaTestApp.Editor
{
    /// <summary>
    /// Headless build entry point:
    /// Unity -batchmode -nographics -projectPath testapp -buildTarget Android
    ///       -executeMethod MochaTestApp.Editor.BuildAndroid.Build -quit
    /// </summary>
    public static class BuildAndroid
    {
        private const string OutputPath = "Build/mocha-testapp.apk";
        private const string ScenePath = "Assets/Scenes/MochaTest.unity";

        public static void Build()
        {
            try
            {
                ConfigureProject();
                BuildReport report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    scenes = new[] { ScenePath },
                    locationPathName = OutputPath,
                    target = BuildTarget.Android,
                    options = BuildOptions.Development | BuildOptions.AllowDebugging,
                });

                BuildSummary summary = report.summary;
                Debug.Log($"[MochaTestApp] build result={summary.result} " +
                          $"size={summary.totalSize} errors={summary.totalErrors} " +
                          $"output={Path.GetFullPath(OutputPath)}");

                if (summary.result != BuildResult.Succeeded)
                {
                    Debug.LogError("[MochaTestApp] Android build failed");
                    EditorApplication.Exit(1);
                }

                EditorApplication.Exit(0);
            }
            catch (Exception exception)
            {
                Debug.LogException(exception);
                EditorApplication.Exit(1);
            }
        }

        private static void ConfigureProject()
        {
            if (!File.Exists(ScenePath))
            {
                SceneBootstrap.CreateScene();
                AssetDatabase.SaveAssets();
                AssetDatabase.Refresh();
            }

            PlayerSettings.companyName = "Mocha";
            PlayerSettings.productName = "MochaTestApp";
            PlayerSettings.SetApplicationIdentifier(
                NamedBuildTarget.Android, "dev.mocha.testapp");

            PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel26;
            PlayerSettings.Android.targetSdkVersion = AndroidSdkVersions.AndroidApiLevelAuto;
            // Unity 6 removed Android x86_64 support ("Target architecture x86_64 is
            // no longer supported and has been removed"). Requesting X86_64 silently
            // produced an ARM64-only APK. Build ARM64 only and run it on the x86_64
            // emulator through the platform's ARM native bridge.
            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;

            PlayerSettings.SetScriptingBackend(
                NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
            PlayerSettings.SetIl2CppCompilerConfiguration(
                NamedBuildTarget.Android, Il2CppCompilerConfiguration.Release);
            PlayerSettings.SetManagedStrippingLevel(
                NamedBuildTarget.Android, ManagedStrippingLevel.Low);
            PlayerSettings.SetApiCompatibilityLevel(
                NamedBuildTarget.Android, ApiCompatibilityLevel.NET_Standard);

            EditorUserBuildSettings.buildAppBundle = false;
            EditorUserBuildSettings.exportAsGoogleAndroidProject = false;

            string[] scenes = new[] { ScenePath };
            EditorBuildSettings.scenes = scenes
                .Select(path => new EditorBuildSettingsScene(path, true))
                .ToArray();

            Debug.Log($"[MochaTestApp] arch={PlayerSettings.Android.targetArchitectures} " +
                      $"backend={PlayerSettings.GetScriptingBackend(NamedBuildTarget.Android)} " +
                      $"unity={Application.unityVersion}");
        }
    }
}
