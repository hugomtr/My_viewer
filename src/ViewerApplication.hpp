#pragma once

#include "utils/GLFWHandle.hpp"
#include "utils/cameras.hpp"
#include "utils/filesystem.hpp"
#include "utils/shaders.hpp"

#include <random>
#include <tiny_gltf.h>

struct Locations
{
  int uModelViewProjMatrix;
  int uModelViewMatrix;
  int uModelMatrix;
  int uNormalMatrix;
  int uLightDirection;
  int uLightIntensity;
  int uBaseColorTexture;
  int uBaseColorFactor;
  int uMetallicRoughnessTexture;
  int uMetallicFactor;
  int uRoughnessFactor;
  int uEmissiveTexture;
  int uEmissiveFactor;
  int uOcclusionTexture;
  int uOcclusionStrength;
  int uApplyOcclusion;
};

class ViewerApplication
{
public:
  ViewerApplication(const fs::path &appPath, uint32_t width, uint32_t height,
      const fs::path &gltfFile, const std::vector<float> &lookatArgs,
      const std::string &vertexShader, const std::string &fragmentShader,
      const fs::path &output);

  int run();

private:
  // A range of indices in a vector containing Vertex Array Objects
  struct VaoRange
  {
    GLsizei begin; // Index of first element in vertexArrayObjects
    GLsizei count; // Number of elements in range
  };

  bool loadGltfFile(tinygltf::Model &model);

  std::vector<GLuint> createTextureObjects(const tinygltf::Model &model) const;

  std::vector<GLuint> createBufferObjects(const tinygltf::Model &model) const;

  void loadLocations(GLuint, Locations &);
  int createGBuffer();
  void RenderGbuffer();
  void renderQuad();

  std::vector<GLuint> createVertexArrayObjects(const tinygltf::Model &model,
      const std::vector<GLuint> &bufferObjects,
      std::vector<VaoRange> &meshToVertexArrays) const;

  GLsizei m_nWindowWidth = 1280;
  GLsizei m_nWindowHeight = 720;

  const fs::path m_AppPath;
  const std::string m_AppName;
  const fs::path m_ShadersRootPath;

  fs::path m_gltfFilePath;
  std::string m_vertexShader = "forward.vs.glsl";
  std::string m_fragmentShader = "pbr_directional_light.fs.glsl";
  std::string m_vertexShaderGBuffer = "deferred_gbuffer.vs.glsl";
  std::string m_fragmentShaderGBuffer = "deferred_gbuffer.fs.glsl";
  std::string m_vertexShaderDShading = "deferred_shading.vs.glsl";
  std::string m_fragmentShaderDShading = "deferred_shading.fs.glsl";
  std::string m_vertexShaderSsao = "ssao.vs.glsl";
  std::string m_fragmentShaderSsao = "ssao.fs.glsl";
  std::string m_fragmentShaderSsaoBlur = "ssao_blur.fs.glsl";

  bool m_hasUserCamera = false;
  Camera m_userCamera;

  fs::path m_OutputPath;

  // Order is important here, see comment below
  const std::string m_ImGuiIniFilename;
  // Last to be initialized, first to be destroyed:
  GLFWHandle m_GLFWHandle{int(m_nWindowWidth), int(m_nWindowHeight),
      "glTF Viewer",
      m_OutputPath.empty()}; // show the window only if m_OutputPath is empty
  /*
    ! THE ORDER OF DECLARATION OF MEMBER VARIABLES IS IMPORTANT !
    - m_ImGuiIniFilename.c_str() will be used by ImGUI in ImGui::Shutdown, which
    will be called in destructor of m_GLFWHandle. So we must declare
    m_ImGuiIniFilename before m_GLFWHandle so that m_ImGuiIniFilename
    destructor is called after.
    - m_GLFWHandle must be declared before the creation of any object managing
    OpenGL resources (e.g. GLProgram, GLShader) because it is responsible for
    the creation of a GLFW windows and thus a GL context which must exists
    before most of OpenGL function calls.
  */
  unsigned int quadVAO = 0;
  unsigned int quadVBO;

  // texture for gbuffer
  unsigned int gbuffer;
  unsigned int gPosition;
  unsigned int gNormal;
  unsigned int gDiffuse;
  unsigned int gMetallic;
  unsigned int gEmissive;
  unsigned int gOcclusion;

  // ssao
  std::vector<glm::vec3> ssaoKernel;
  std::vector<glm::vec3> ssaoNoise;
  unsigned int noiseTexture;

  unsigned int ssaoFBO, ssaoBlurFBO;
  unsigned int ssaoColorBuffer, ssaoColorBufferBlur;

  float lerp(float a, float b, float f) { return a + f * (b - a); };
  void ssaoPrepare();
};
