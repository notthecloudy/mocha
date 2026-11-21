using System.IO;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace MochaTestApp
{
    internal static class SceneBootstrap
    {
        internal static void CreateScene()
        {
            Scene scene = EditorSceneManager.NewScene(
                NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var cameraObject = new GameObject("Camera");
            Camera camera = cameraObject.AddComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(0.05f, 0.05f, 0.12f, 1f);
            cameraObject.tag = "MainCamera";
            cameraObject.transform.position = new Vector3(0f, 1f, -4f);
            cameraObject.transform.rotation = Quaternion.identity;

            var lightObject = new GameObject("Directional Light");
            Light light = lightObject.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1f;
            lightObject.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = "MochaTestCube";
            cube.transform.position = Vector3.zero;
            cube.AddComponent<TestBootstrap>();

            Directory.CreateDirectory("Assets/Scenes");
            EditorSceneManager.SaveScene(scene, "Assets/Scenes/MochaTest.unity");
        }
    }
}
