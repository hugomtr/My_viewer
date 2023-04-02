#include "ViewerApplication.hpp"

#include <iostream>
#include <numeric>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/io.hpp>

#include "utils/cameras.hpp"
#include "utils/gltf.hpp"
#include "utils/images.hpp"

#include <stb_image_write.h>
#include <tiny_gltf.h>

void keyCallback(
    GLFWwindow *window, int key, int scancode, int action, int mods)
{
  if (key == GLFW_KEY_ESCAPE && action == GLFW_RELEASE) {
    glfwSetWindowShouldClose(window, 1);
  }
}

int ViewerApplication::run()
{

  // Setup OpenGL state for rendering
  glEnable(GL_DEPTH_TEST);

  bool deferred_rendering = false;
  bool render_gbuffer_content = false;
  bool render_with_ssao = false;
  int render_gbuffer_id = 0;

  const auto glslProgram = compileProgram({m_ShadersRootPath / m_vertexShader,
      m_ShadersRootPath / m_fragmentShader});

  const auto glslProgramdGeometry =
      compileProgram({m_ShadersRootPath / m_vertexShaderGBuffer,
          m_ShadersRootPath / m_fragmentShaderGBuffer});

  const auto glslProgramdShading =
      compileProgram({m_ShadersRootPath / m_vertexShaderDShading,
          m_ShadersRootPath / m_fragmentShaderDShading});

  const auto glslProgramdSsao =
      compileProgram({m_ShadersRootPath / m_vertexShaderSsao,
          m_ShadersRootPath / m_fragmentShaderSsao});

  const auto glslProgramdSsaoBlur =
      compileProgram({m_ShadersRootPath / m_vertexShaderSsao,
          m_ShadersRootPath / m_fragmentShaderSsaoBlur});

  Locations location;
  loadLocations(glslProgram.glId(), location);

  Locations locationgbuffer;
  loadLocations(glslProgramdGeometry.glId(), locationgbuffer);

  Locations locationDShading;
  loadLocations(glslProgramdShading.glId(), locationDShading);

  Locations locationSsao;
  loadLocations(glslProgramdSsao.glId(), locationSsao);

  Locations locationSsaoBlur;
  loadLocations(glslProgramdSsaoBlur.glId(), locationSsaoBlur);

  tinygltf::Model model;
  if (!loadGltfFile(model)) {
    return -1;
  }
  glm::vec3 bboxMin, bboxMax;
  computeSceneBounds(model, bboxMin, bboxMax);

  // Build projection matrix
  const auto diag = bboxMax - bboxMin;
  auto maxDistance = glm::length(diag);
  const auto projMatrix =
      glm::perspective(70.f, float(m_nWindowWidth) / m_nWindowHeight,
          0.001f * maxDistance, 1.5f * maxDistance);

  std::unique_ptr<CameraController> cameraController =
      std::make_unique<TrackballCameraController>(
          m_GLFWHandle.window(), 0.5f * maxDistance);
  if (m_hasUserCamera) {
    cameraController->setCamera(m_userCamera);
  } else {
    const auto center = 0.5f * (bboxMax + bboxMin);
    const auto up = glm::vec3(0, 1, 0);
    const auto eye =
        diag.z > 0 ? center + diag : center + 2.f * glm::cross(diag, up);
    cameraController->setCamera(Camera{eye, center, up});
  }

  // Init light parameters
  glm::vec3 lightDirection(1, 1, 1);
  glm::vec3 lightIntensity(1, 1, 1);
  bool lightFromCamera = false;
  bool applyOcclusion = true;

  // Load textures
  const auto textureObjects = createTextureObjects(model);

  GLuint whiteTexture = 0;

  // Create white texture for object with no base color texture
  glGenTextures(1, &whiteTexture);
  glBindTexture(GL_TEXTURE_2D, whiteTexture);
  float white[] = {1, 1, 1, 1};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_REPEAT);
  glBindTexture(GL_TEXTURE_2D, 0);

  const auto bufferObjects = createBufferObjects(model);

  std::vector<VaoRange> meshToVertexArrays;
  const auto vertexArrayObjects =
      createVertexArrayObjects(model, bufferObjects, meshToVertexArrays);

  // G buffer preparation
  createGBuffer();
  // SSAO preparation
  ssaoPrepare();

  const auto bindMaterial = [&](const auto materialIndex,
                                const Locations &location) {
    if (materialIndex >= 0) {
      const auto &material = model.materials[materialIndex];
      const auto &pbrMetallicRoughness = material.pbrMetallicRoughness;
      if (location.uBaseColorFactor >= 0) {
        glUniform4f(location.uBaseColorFactor,
            (float)pbrMetallicRoughness.baseColorFactor[0],
            (float)pbrMetallicRoughness.baseColorFactor[1],
            (float)pbrMetallicRoughness.baseColorFactor[2],
            (float)pbrMetallicRoughness.baseColorFactor[3]);
      }
      if (location.uBaseColorTexture >= 0) {
        auto textureObject = whiteTexture;
        if (pbrMetallicRoughness.baseColorTexture.index >= 0) {
          const auto &texture =
              model.textures[pbrMetallicRoughness.baseColorTexture.index];
          if (texture.source >= 0) {
            textureObject = textureObjects[texture.source];
          }
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureObject);
        glUniform1i(location.uBaseColorTexture, 0);
      }
      if (location.uMetallicFactor >= 0) {
        glUniform1f(location.uMetallicFactor,
            (float)pbrMetallicRoughness.metallicFactor);
      }
      if (location.uRoughnessFactor >= 0) {
        glUniform1f(location.uRoughnessFactor,
            (float)pbrMetallicRoughness.roughnessFactor);
      }
      if (location.uMetallicRoughnessTexture >= 0) {
        auto textureObject = 0u;
        if (pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
          const auto &texture =
              model.textures[pbrMetallicRoughness.metallicRoughnessTexture
                                 .index];
          if (texture.source >= 0) {
            textureObject = textureObjects[texture.source];
          }
        }

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textureObject);
        glUniform1i(location.uMetallicRoughnessTexture, 1);
      }
      if (location.uEmissiveFactor >= 0) {
        glUniform3f(location.uEmissiveFactor, (float)material.emissiveFactor[0],
            (float)material.emissiveFactor[1],
            (float)material.emissiveFactor[2]);
      }
      if (location.uEmissiveTexture >= 0) {
        auto textureObject = 0u;
        if (material.emissiveTexture.index >= 0) {
          const auto &texture = model.textures[material.emissiveTexture.index];
          if (texture.source >= 0) {
            textureObject = textureObjects[texture.source];
          }
        }

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, textureObject);
        glUniform1i(location.uEmissiveTexture, 2);
      }
      if (location.uOcclusionStrength >= 0) {
        glUniform1f(location.uOcclusionStrength,
            (float)material.occlusionTexture.strength);
      }
      if (location.uOcclusionTexture >= 0) {
        auto textureObject = whiteTexture;
        if (material.occlusionTexture.index >= 0) {
          const auto &texture = model.textures[material.occlusionTexture.index];
          if (texture.source >= 0) {
            textureObject = textureObjects[texture.source];
          }
        }

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, textureObject);
        glUniform1i(location.uOcclusionTexture, 3);
      }
    } else {
      // Apply default material
      // Defined here:
      // https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/README.md#reference-pbrmetallicroughness3
      if (location.uBaseColorFactor >= 0) {
        glUniform4f(location.uBaseColorFactor, 1, 1, 1, 1);
      }
      if (location.uBaseColorTexture >= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, whiteTexture);
        glUniform1i(location.uBaseColorTexture, 0);
      }
      if (location.uMetallicFactor >= 0) {
        glUniform1f(location.uMetallicFactor, 1.f);
      }
      if (location.uRoughnessFactor >= 0) {
        glUniform1f(location.uRoughnessFactor, 1.f);
      }
      if (location.uMetallicRoughnessTexture >= 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(location.uMetallicRoughnessTexture, 1);
      }
      if (location.uEmissiveFactor >= 0) {
        glUniform3f(location.uEmissiveFactor, 0.f, 0.f, 0.f);
      }
      if (location.uEmissiveTexture >= 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(location.uEmissiveTexture, 2);
      }
      if (location.uOcclusionStrength >= 0) {
        glUniform1f(location.uOcclusionStrength, 0.f);
      }
      if (location.uOcclusionTexture >= 0) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(location.uOcclusionTexture, 3);
      }
    }
  };

  const auto drawLight = [&](const Camera &camera, const Locations &location) {
    const auto viewMatrix = camera.getViewMatrix();

    if (location.uLightDirection >= 0) {
      if (lightFromCamera) {
        glUniform3f(location.uLightDirection, 0, 0, 1);
      } else {
        const auto lightDirectionInViewSpace = glm::normalize(
            glm::vec3(viewMatrix * glm::vec4(lightDirection, 0.)));
        glUniform3f(location.uLightDirection, lightDirectionInViewSpace[0],
            lightDirectionInViewSpace[1], lightDirectionInViewSpace[2]);
      }
    }

    if (location.uLightIntensity >= 0) {
      glUniform3f(location.uLightIntensity, lightIntensity[0],
          lightIntensity[1], lightIntensity[2]);
    }

    if (location.uApplyOcclusion >= 0) {
      glUniform1i(location.uApplyOcclusion, applyOcclusion);
    }
  };

  // Lambda function to draw the scene
  const auto drawScene = [&](const Camera &camera, const Locations &location,
                             bool light = true) {
    glViewport(0, 0, m_nWindowWidth, m_nWindowHeight);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const auto viewMatrix = camera.getViewMatrix();

    if (light)
      drawLight(camera, location);

    // The recursive function that should draw a node
    // We use a std::function because a simple lambda cannot be recursive
    const std::function<void(int, const glm::mat4 &)> drawNode =
        [&](int nodeIdx, const glm::mat4 &parentMatrix) {
          const auto &node = model.nodes[nodeIdx];
          const auto modelMatrix = getLocalToWorldMatrix(node, parentMatrix);

          // If the node references a mesh (a node can also reference a
          // camera, or a light)
          if (node.mesh >= 0) {
            const auto mvMatrix =
                viewMatrix * modelMatrix; // Also called localToCamera matrix
            const auto mvpMatrix =
                projMatrix * mvMatrix; // Also called localToScreen matrix
            // Normal matrix is necessary to maintain normal vectors
            // orthogonal to tangent vectors
            // https://www.lighthouse3d.com/tutorials/glsl-12-tutorial/the-normal-matrix/
            const auto normalMatrix = glm::transpose(glm::inverse(mvMatrix));

            if (location.uModelMatrix >= 0)
              glUniformMatrix4fv(location.uModelMatrix, 1, GL_FALSE,
                  glm::value_ptr(modelMatrix));
            if (location.uModelViewProjMatrix >= 0)
              glUniformMatrix4fv(location.uModelViewProjMatrix, 1, GL_FALSE,
                  glm::value_ptr(mvpMatrix));
            if (location.uModelViewMatrix >= 0)
              glUniformMatrix4fv(location.uModelViewMatrix, 1, GL_FALSE,
                  glm::value_ptr(mvMatrix));
            if (location.uNormalMatrix >= 0)
              glUniformMatrix4fv(location.uNormalMatrix, 1, GL_FALSE,
                  glm::value_ptr(normalMatrix));

            const auto &mesh = model.meshes[node.mesh];
            const auto &vaoRange = meshToVertexArrays[node.mesh];
            for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
              const auto vao = vertexArrayObjects[vaoRange.begin + pIdx];
              const auto &primitive = mesh.primitives[pIdx];

              bindMaterial(primitive.material, location);

              glBindVertexArray(vao);
              if (primitive.indices >= 0) {
                const auto &accessor = model.accessors[primitive.indices];
                const auto &bufferView = model.bufferViews[accessor.bufferView];
                const auto byteOffset =
                    accessor.byteOffset + bufferView.byteOffset;
                glDrawElements(primitive.mode, GLsizei(accessor.count),
                    accessor.componentType, (const GLvoid *)byteOffset);
              } else {
                // Take first accessor to get the count
                const auto accessorIdx = (*begin(primitive.attributes)).second;
                const auto &accessor = model.accessors[accessorIdx];
                glDrawArrays(primitive.mode, 0, GLsizei(accessor.count));
              }
            }
          }

          // Draw children
          for (const auto childNodeIdx : node.children) {
            drawNode(childNodeIdx, modelMatrix);
          }
        };

    // Draw the scene referenced by gltf file
    if (model.defaultScene >= 0) {
      for (const auto nodeIdx : model.scenes[model.defaultScene].nodes) {
        drawNode(nodeIdx, glm::mat4(1));
      }
    }
  };

  // If we want to render in an image
  if (!m_OutputPath.empty()) {
    const auto numComponents = 3;
    std::vector<unsigned char> pixels(
        m_nWindowWidth * m_nWindowHeight * numComponents);
    renderToImage(
        m_nWindowWidth, m_nWindowHeight, numComponents, pixels.data(), [&]() {
          const auto camera = cameraController->getCamera();
          // drawScene(camera, location);
        });
    // OpenGL has not the same convention for image axis than most image
    // formats, so we flip on the Y axis
    flipImageYAxis(
        m_nWindowWidth, m_nWindowHeight, numComponents, pixels.data());

    // Write png on disk
    const auto strPath = m_OutputPath.string();
    stbi_write_png(
        strPath.c_str(), m_nWindowWidth, m_nWindowHeight, 3, pixels.data(), 0);

    return 0; // Exit, in that mode we don't want to run interactive viewer
  }

  // Loop until the user closes the window
  for (auto iterationCount = 0u; !m_GLFWHandle.shouldClose();
       ++iterationCount) {
    const auto seconds = glfwGetTime();

    const auto camera = cameraController->getCamera();
    if (deferred_rendering) {
      // Geometry pass
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glslProgramdGeometry.use();
      drawScene(camera, locationgbuffer, false);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

      //
      if (render_gbuffer_content) {
        // render G buffer content
        //  TO DO call render g buffer function because now the code render only
        //  one texture
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);

        glReadBuffer(GL_COLOR_ATTACHMENT0 + render_gbuffer_id - 1);
        glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, 0, 0,
            m_nWindowWidth, m_nWindowHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
      } else {

        //
        // Do shading calculations on gbuffer and render the results
        if (render_with_ssao) {
          glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
          glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
          glslProgramdSsao.use();
          // Send kernel + rotation
          for (unsigned int i = 0; i < 64; ++i) {
            auto name = "samples[" + std::to_string(i) + "]";
            glUniform3fv(
                glGetUniformLocation(glslProgramdSsao.glId(), name.c_str()), 1,
                &ssaoKernel[i][0]);
          }
          glUniformMatrix4fv(locationSsao.uModelViewProjMatrix, 1, GL_FALSE,
              glm::value_ptr(projMatrix));
          glUniform1f(
              glGetUniformLocation(glslProgramdSsao.glId(), "m_nWindowWidth"),
              (float)m_nWindowWidth);
          glUniform1f(
              glGetUniformLocation(glslProgramdSsao.glId(), "m_nWindowHeight"),
              (float)m_nWindowHeight);
          glUniform1i(
              glGetUniformLocation(glslProgramdSsao.glId(), "gPosition"), 0);
          glUniform1i(
              glGetUniformLocation(glslProgramdSsao.glId(), "gNormal"), 1);
          glUniform1i(
              glGetUniformLocation(glslProgramdSsao.glId(), "texNoise"), 2);
          glActiveTexture(GL_TEXTURE0);
          glBindTexture(GL_TEXTURE_2D, gPosition);
          glActiveTexture(GL_TEXTURE1);
          glBindTexture(GL_TEXTURE_2D, gNormal);
          glActiveTexture(GL_TEXTURE2);
          glBindTexture(GL_TEXTURE_2D, noiseTexture);
          renderQuad();
          glBindFramebuffer(GL_FRAMEBUFFER, 0);

          // 3. blur SSAO texture to remove noise
          glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
          glClear(GL_COLOR_BUFFER_BIT);
          glslProgramdSsaoBlur.use();
          glUniform1i(
              glGetUniformLocation(glslProgramdSsaoBlur.glId(), "ssaoInput"),
              0);
          glActiveTexture(GL_TEXTURE0);
          glBindTexture(GL_TEXTURE_2D, ssaoColorBuffer); // <- Error Here
          renderQuad();
          glBindFramebuffer(GL_FRAMEBUFFER, 0);

          glslProgramdShading.use();
          glUniform1i(glGetUniformLocation(
                          glslProgramdShading.glId(), "ssaoColorBufferBlur"),
              6);
          glActiveTexture(GL_TEXTURE6);
          glBindTexture(GL_TEXTURE_2D, ssaoColorBufferBlur);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glslProgramdShading.use();
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gPosition"), 0);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gNormal"), 1);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gDiffuse"), 2);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gMetallic"), 3);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gEmissive"), 4);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "gOcclusion"), 5);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gPosition);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gNormal);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gDiffuse);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, gMetallic);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, gEmissive);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, gOcclusion);
        glUniform1i(
            glGetUniformLocation(glslProgramdShading.glId(), "with_ssao"),
            (int)render_with_ssao);
        drawLight(camera, locationDShading);
        renderQuad(); // render the scene on the screen

        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, 0, 0,
            m_nWindowWidth, m_nWindowHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
      }
    } else {
      // forward render
      glslProgram.use();
      drawScene(camera, location);
    }

    // GUI code:
    imguiNewFrame();

    {
      ImGui::Begin("GUI");
      ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
          1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
      if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("eye: %.3f %.3f %.3f", camera.eye().x, camera.eye().y,
            camera.eye().z);
        ImGui::Text("center: %.3f %.3f %.3f", camera.center().x,
            camera.center().y, camera.center().z);
        ImGui::Text(
            "up: %.3f %.3f %.3f", camera.up().x, camera.up().y, camera.up().z);

        ImGui::Text("front: %.3f %.3f %.3f", camera.front().x, camera.front().y,
            camera.front().z);
        ImGui::Text("left: %.3f %.3f %.3f", camera.left().x, camera.left().y,
            camera.left().z);

        if (ImGui::Button("CLI camera args to clipboard")) {
          std::stringstream ss;
          ss << "--lookat " << camera.eye().x << "," << camera.eye().y << ","
             << camera.eye().z << "," << camera.center().x << ","
             << camera.center().y << "," << camera.center().z << ","
             << camera.up().x << "," << camera.up().y << "," << camera.up().z;
          const auto str = ss.str();
          glfwSetClipboardString(m_GLFWHandle.window(), str.c_str());
        }

        static int cameraControllerType = 0;
        const auto cameraControllerTypeChanged =
            ImGui::RadioButton("Trackball", &cameraControllerType, 0) ||
            ImGui::RadioButton("First Person", &cameraControllerType, 1);
        if (cameraControllerTypeChanged) {
          const auto currentCamera = cameraController->getCamera();
          if (cameraControllerType == 0) {
            cameraController = std::make_unique<TrackballCameraController>(
                m_GLFWHandle.window(), 0.5f * maxDistance);
          } else {
            cameraController = std::make_unique<FirstPersonCameraController>(
                m_GLFWHandle.window(), 0.5f * maxDistance);
          }
          cameraController->setCamera(currentCamera);
        }
      }
      if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float lightTheta = 0.f;
        static float lightPhi = 0.f;

        if (ImGui::SliderFloat("theta", &lightTheta, 0, glm::pi<float>()) ||
            ImGui::SliderFloat("phi", &lightPhi, 0, 2.f * glm::pi<float>())) {
          const auto sinPhi = glm::sin(lightPhi);
          const auto cosPhi = glm::cos(lightPhi);
          const auto sinTheta = glm::sin(lightTheta);
          const auto cosTheta = glm::cos(lightTheta);
          lightDirection =
              glm::vec3(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
        }

        static glm::vec3 lightColor(1.f, 1.f, 1.f);
        static float lightIntensityFactor = 1.f;

        if (ImGui::ColorEdit3("color", (float *)&lightColor) ||
            ImGui::InputFloat("intensity", &lightIntensityFactor)) {
          lightIntensity = lightColor * lightIntensityFactor;
        }

        ImGui::Checkbox("light from camera", &lightFromCamera);
        ImGui::Checkbox("apply occlusion", &applyOcclusion);
        ImGui::Checkbox("Deferred Rendering", &deferred_rendering);
        ImGui::Checkbox("with SSAO", &render_with_ssao);

        if (ImGui::CollapsingHeader(
                "Render G Buffer", ImGuiTreeNodeFlags_DefaultOpen)) {

          ImGui::RadioButton("No rendering G buffer", &render_gbuffer_id, 0);
          ImGui::RadioButton("Render position", &render_gbuffer_id, 1);
          ImGui::RadioButton("Render Normal", &render_gbuffer_id, 2);
          ImGui::RadioButton("Render Diffuse", &render_gbuffer_id, 3);
          ImGui::RadioButton("Render Metallic", &render_gbuffer_id, 4);
          ImGui::RadioButton("Render Emissive", &render_gbuffer_id, 5);
          ImGui::RadioButton("Render Occlusion", &render_gbuffer_id, 6);

          render_gbuffer_content = (render_gbuffer_id > 0) ? true : false;
        }
      }
      ImGui::End();
    }

    imguiRenderFrame();

    glfwPollEvents(); // Poll for and process events

    auto ellapsedTime = glfwGetTime() - seconds;
    auto guiHasFocus =
        ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
    if (!guiHasFocus) {
      cameraController->update(float(ellapsedTime));
    }

    m_GLFWHandle.swapBuffers(); // Swap front and back buffers
  }

  // TODO clean up allocated GL data

  return 0;
}

bool ViewerApplication::loadGltfFile(tinygltf::Model &model)
{
  std::clog << "Loading file " << m_gltfFilePath << std::endl;

  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;

  bool ret =
      loader.LoadASCIIFromFile(&model, &err, &warn, m_gltfFilePath.string());

  if (!warn.empty()) {
    std::cerr << warn << std::endl;
  }

  if (!err.empty()) {
    std::cerr << err << std::endl;
  }

  if (!ret) {
    std::cerr << "Failed to parse glTF file" << std::endl;
    return false;
  }

  return true;
}

std::vector<GLuint> ViewerApplication::createTextureObjects(
    const tinygltf::Model &model) const
{
  std::vector<GLuint> textureObjects(model.textures.size(), 0);

  // default sampler:
  // https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/README.md#texturesampler
  // "When undefined, a sampler with repeat wrapping and auto filtering should
  // be used."
  tinygltf::Sampler defaultSampler;
  defaultSampler.minFilter = GL_LINEAR;
  defaultSampler.magFilter = GL_LINEAR;
  defaultSampler.wrapS = GL_REPEAT;
  defaultSampler.wrapT = GL_REPEAT;
  defaultSampler.wrapR = GL_REPEAT;

  glActiveTexture(GL_TEXTURE0);

  glGenTextures(GLsizei(model.textures.size()), textureObjects.data());
  for (size_t i = 0; i < model.textures.size(); ++i) {
    const auto &texture = model.textures[i];
    assert(texture.source >= 0);
    const auto &image = model.images[texture.source];

    const auto &sampler =
        texture.sampler >= 0 ? model.samplers[texture.sampler] : defaultSampler;
    glBindTexture(GL_TEXTURE_2D, textureObjects[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0,
        GL_RGBA, image.pixel_type, image.image.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
        sampler.minFilter != -1 ? sampler.minFilter : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
        sampler.magFilter != -1 ? sampler.magFilter : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler.wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler.wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, sampler.wrapR);

    if (sampler.minFilter == GL_NEAREST_MIPMAP_NEAREST ||
        sampler.minFilter == GL_NEAREST_MIPMAP_LINEAR ||
        sampler.minFilter == GL_LINEAR_MIPMAP_NEAREST ||
        sampler.minFilter == GL_LINEAR_MIPMAP_LINEAR) {
      glGenerateMipmap(GL_TEXTURE_2D);
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  return textureObjects;
}

std::vector<GLuint> ViewerApplication::createBufferObjects(
    const tinygltf::Model &model) const
{
  std::vector<GLuint> bufferObjects(model.buffers.size(), 0);

  glGenBuffers(GLsizei(model.buffers.size()), bufferObjects.data());
  for (size_t i = 0; i < model.buffers.size(); ++i) {
    glBindBuffer(GL_ARRAY_BUFFER, bufferObjects[i]);
    glBufferStorage(GL_ARRAY_BUFFER, model.buffers[i].data.size(),
        model.buffers[i].data.data(), 0);
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  return bufferObjects;
}

std::vector<GLuint> ViewerApplication::createVertexArrayObjects(
    const tinygltf::Model &model, const std::vector<GLuint> &bufferObjects,
    std::vector<VaoRange> &meshToVertexArrays) const
{
  std::vector<GLuint> vertexArrayObjects; // We don't know the size yet

  // For each mesh of model we keep its range of VAOs
  meshToVertexArrays.resize(model.meshes.size());

  const GLuint VERTEX_ATTRIB_POSITION_IDX = 0;
  const GLuint VERTEX_ATTRIB_NORMAL_IDX = 1;
  const GLuint VERTEX_ATTRIB_TEXCOORD0_IDX = 2;

  for (size_t i = 0; i < model.meshes.size(); ++i) {
    const auto &mesh = model.meshes[i];

    auto &vaoRange = meshToVertexArrays[i];
    vaoRange.begin =
        GLsizei(vertexArrayObjects.size()); // Range for this mesh will be at
                                            // the end of vertexArrayObjects
    vaoRange.count =
        GLsizei(mesh.primitives.size()); // One VAO for each primitive

    // Add enough elements to store our VAOs identifiers
    vertexArrayObjects.resize(
        vertexArrayObjects.size() + mesh.primitives.size());

    glGenVertexArrays(vaoRange.count, &vertexArrayObjects[vaoRange.begin]);
    for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
      const auto vao = vertexArrayObjects[vaoRange.begin + pIdx];
      const auto &primitive = mesh.primitives[pIdx];
      glBindVertexArray(vao);
      { // POSITION attribute
        // scope, so we can declare const variable with the same name on each
        // scope
        const auto iterator = primitive.attributes.find("POSITION");
        if (iterator != end(primitive.attributes)) {
          const auto accessorIdx = (*iterator).second;
          const auto &accessor = model.accessors[accessorIdx];
          const auto &bufferView = model.bufferViews[accessor.bufferView];
          const auto bufferIdx = bufferView.buffer;

          glEnableVertexAttribArray(VERTEX_ATTRIB_POSITION_IDX);
          assert(GL_ARRAY_BUFFER == bufferView.target);
          // Theorically we could also use bufferView.target, but it is safer
          // Here it is important to know that the next call
          // (glVertexAttribPointer) use what is currently bound
          glBindBuffer(GL_ARRAY_BUFFER, bufferObjects[bufferIdx]);

          // tinygltf converts strings type like "VEC3, "VEC2" to the number of
          // components, stored in accessor.type
          const auto byteOffset = accessor.byteOffset + bufferView.byteOffset;
          glVertexAttribPointer(VERTEX_ATTRIB_POSITION_IDX, accessor.type,
              accessor.componentType, GL_FALSE, GLsizei(bufferView.byteStride),
              (const GLvoid *)byteOffset);
        }
      }
      // todo Refactor to remove code duplication (loop over "POSITION",
      // "NORMAL" and their corresponding VERTEX_ATTRIB_*)
      { // NORMAL attribute
        const auto iterator = primitive.attributes.find("NORMAL");
        if (iterator != end(primitive.attributes)) {
          const auto accessorIdx = (*iterator).second;
          const auto &accessor = model.accessors[accessorIdx];
          const auto &bufferView = model.bufferViews[accessor.bufferView];
          const auto bufferIdx = bufferView.buffer;

          glEnableVertexAttribArray(VERTEX_ATTRIB_NORMAL_IDX);
          assert(GL_ARRAY_BUFFER == bufferView.target);
          glBindBuffer(GL_ARRAY_BUFFER, bufferObjects[bufferIdx]);
          glVertexAttribPointer(VERTEX_ATTRIB_NORMAL_IDX, accessor.type,
              accessor.componentType, GL_FALSE, GLsizei(bufferView.byteStride),
              (const GLvoid *)(accessor.byteOffset + bufferView.byteOffset));
        }
      }
      { // TEXCOORD_0 attribute
        const auto iterator = primitive.attributes.find("TEXCOORD_0");
        if (iterator != end(primitive.attributes)) {
          const auto accessorIdx = (*iterator).second;
          const auto &accessor = model.accessors[accessorIdx];
          const auto &bufferView = model.bufferViews[accessor.bufferView];
          const auto bufferIdx = bufferView.buffer;

          glEnableVertexAttribArray(VERTEX_ATTRIB_TEXCOORD0_IDX);
          assert(GL_ARRAY_BUFFER == bufferView.target);
          glBindBuffer(GL_ARRAY_BUFFER, bufferObjects[bufferIdx]);
          glVertexAttribPointer(VERTEX_ATTRIB_TEXCOORD0_IDX, accessor.type,
              accessor.componentType, GL_FALSE, GLsizei(bufferView.byteStride),
              (const GLvoid *)(accessor.byteOffset + bufferView.byteOffset));
        }
      }
      // Index array if defined
      if (primitive.indices >= 0) {
        const auto accessorIdx = primitive.indices;
        const auto &accessor = model.accessors[accessorIdx];
        const auto &bufferView = model.bufferViews[accessor.bufferView];
        const auto bufferIdx = bufferView.buffer;

        assert(GL_ELEMENT_ARRAY_BUFFER == bufferView.target);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
            bufferObjects[bufferIdx]); // Binding the index buffer to
                                       // GL_ELEMENT_ARRAY_BUFFER while the VAO
                                       // is bound is enough to tell OpenGL we
                                       // want to use that index buffer for that
                                       // VAO
      }
    }
  }
  glBindVertexArray(0);

  std::clog << "Number of VAOs: " << vertexArrayObjects.size() << std::endl;

  return vertexArrayObjects;
}

ViewerApplication::ViewerApplication(const fs::path &appPath, uint32_t width,
    uint32_t height, const fs::path &gltfFile,
    const std::vector<float> &lookatArgs, const std::string &vertexShader,
    const std::string &fragmentShader, const fs::path &output) :
    m_nWindowWidth(width),
    m_nWindowHeight(height),
    m_AppPath{appPath},
    m_AppName{m_AppPath.stem().string()},
    m_ImGuiIniFilename{m_AppName + ".imgui.ini"},
    m_ShadersRootPath{m_AppPath.parent_path() / "shaders"},
    m_gltfFilePath{gltfFile},
    m_OutputPath{output}
{
  if (!lookatArgs.empty()) {
    m_hasUserCamera = true;
    m_userCamera =
        Camera{glm::vec3(lookatArgs[0], lookatArgs[1], lookatArgs[2]),
            glm::vec3(lookatArgs[3], lookatArgs[4], lookatArgs[5]),
            glm::vec3(lookatArgs[6], lookatArgs[7], lookatArgs[8])};
  }

  if (!vertexShader.empty()) {
    m_vertexShader = vertexShader;
  }

  if (!fragmentShader.empty()) {
    m_fragmentShader = fragmentShader;
  }

  ImGui::GetIO().IniFilename =
      m_ImGuiIniFilename.c_str(); // At exit, ImGUI will store its windows
                                  // positions in this file

  glfwSetKeyCallback(m_GLFWHandle.window(), keyCallback);

  printGLVersion();
}

void ViewerApplication::loadLocations(GLuint ID, Locations &locations)
{
  locations.uModelViewProjMatrix =
      glGetUniformLocation(ID, "uModelViewProjMatrix");
  locations.uModelViewMatrix = glGetUniformLocation(ID, "uModelViewMatrix");
  locations.uNormalMatrix = glGetUniformLocation(ID, "uNormalMatrix");
  locations.uModelMatrix = glGetUniformLocation(ID, "uModelMatrix");

  locations.uLightDirection = glGetUniformLocation(ID, "uLightDirection");
  locations.uLightIntensity = glGetUniformLocation(ID, "uLightIntensity");

  locations.uBaseColorTexture = glGetUniformLocation(ID, "uBaseColorTexture");
  locations.uBaseColorFactor = glGetUniformLocation(ID, "uBaseColorFactor");

  locations.uMetallicRoughnessTexture =
      glGetUniformLocation(ID, "uMetallicRoughnessTexture");
  locations.uMetallicFactor = glGetUniformLocation(ID, "uMetallicFactor");
  locations.uRoughnessFactor = glGetUniformLocation(ID, "uRoughnessFactor");

  locations.uEmissiveTexture = glGetUniformLocation(ID, "uEmissiveTexture");
  locations.uEmissiveFactor = glGetUniformLocation(ID, "uEmissiveFactor");

  locations.uOcclusionTexture = glGetUniformLocation(ID, "uOcclusionTexture");
  locations.uOcclusionStrength = glGetUniformLocation(ID, "uOcclusionStrength");
  locations.uApplyOcclusion = glGetUniformLocation(ID, "uApplyOcclusion");
}

int ViewerApplication::createGBuffer()
{
  glGenFramebuffers(1, &gbuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, gbuffer);

  // position color buffer
  glGenTextures(1, &gPosition);
  glBindTexture(GL_TEXTURE_2D, gPosition);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPosition, 0);

  // normal color buffer
  glGenTextures(1, &gNormal);
  glBindTexture(GL_TEXTURE_2D, gNormal);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gNormal, 0);

  // color diffuse texture
  glGenTextures(1, &gDiffuse);
  glBindTexture(GL_TEXTURE_2D, gDiffuse);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gDiffuse, 0);

  glGenTextures(1, &gMetallic);
  glBindTexture(GL_TEXTURE_2D, gMetallic);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gMetallic, 0);

  glGenTextures(1, &gEmissive);
  glBindTexture(GL_TEXTURE_2D, gEmissive);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, gEmissive, 0);

  glGenTextures(1, &gOcclusion);
  glBindTexture(GL_TEXTURE_2D, gOcclusion);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RGBA, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, GL_TEXTURE_2D, gOcclusion, 0);

  // tell OpenGL which color attachments we'll use (of this framebuffer) for
  // rendering
  unsigned int attachments[6] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
      GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4,
      GL_COLOR_ATTACHMENT5};
  glDrawBuffers(6, attachments);

  // create and attach depth buffer (renderbuffer)
  unsigned int rboDepth;
  glGenRenderbuffers(1, &rboDepth);
  glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
  glRenderbufferStorage(
      GL_RENDERBUFFER, GL_DEPTH_COMPONENT, m_nWindowWidth, m_nWindowHeight);
  glFramebufferRenderbuffer(
      GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);

  // finally check if framebuffer is complete
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    std::cout << "Framebuffer not complete!" << std::endl;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ViewerApplication::RenderGbuffer()
{
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);

  GLsizei HalfWidth = (GLsizei)(m_nWindowWidth / 2.0f);
  GLsizei HalfHeight = (GLsizei)(m_nWindowHeight / 2.0f);

  glReadBuffer(GL_COLOR_ATTACHMENT0);

  glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, 0, 0, m_nWindowWidth,
      m_nWindowHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, 0, 0, HalfWidth,
      HalfHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glReadBuffer(GL_COLOR_ATTACHMENT1);
  glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, 0, HalfHeight,
      HalfWidth, m_nWindowHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glReadBuffer(GL_COLOR_ATTACHMENT2);

  glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, HalfWidth,
      HalfHeight, m_nWindowWidth, m_nWindowHeight, GL_COLOR_BUFFER_BIT,
      GL_LINEAR);

  glReadBuffer(GL_COLOR_ATTACHMENT3);
  glBlitFramebuffer(0, 0, m_nWindowWidth, m_nWindowHeight, HalfWidth, 0,
      m_nWindowWidth, HalfHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void ViewerApplication::renderQuad()
{
  if (quadVAO == 0) {
    float quadVertices[] = {
        // positions        // texture Coords
        -1.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        -1.0f,
        -1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        0.0f,
        1.0f,
        0.0f,
    };
    // setup plane VAO
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(
        GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
        (void *)(3 * sizeof(float)));
  }
  glBindVertexArray(quadVAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}

void ViewerApplication::ssaoPrepare()
{
  glGenFramebuffers(1, &ssaoFBO);
  glGenFramebuffers(1, &ssaoBlurFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
  // SSAO color buffer
  glGenTextures(1, &ssaoColorBuffer);
  glBindTexture(GL_TEXTURE_2D, ssaoColorBuffer);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RED, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoColorBuffer, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    std::cout << "SSAO Framebuffer not complete!" << std::endl;
  // and blur stage
  glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
  glGenTextures(1, &ssaoColorBufferBlur);
  glBindTexture(GL_TEXTURE_2D, ssaoColorBufferBlur);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_nWindowWidth, m_nWindowHeight, 0,
      GL_RED, GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
      ssaoColorBufferBlur, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    std::cout << "SSAO Blur Framebuffer not complete!" << std::endl;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Ssao Kernel sample
  std::uniform_real_distribution<float> randomFloats(
      0.0, 1.0); // random floats between [0.0, 1.0]
  std::default_random_engine generator;
  for (unsigned int i = 0; i < 64; ++i) {
    glm::vec3 sample(randomFloats(generator) * 2.0 - 1.0,
        randomFloats(generator) * 2.0 - 1.0, randomFloats(generator));
    sample = glm::normalize(sample);
    sample *= randomFloats(generator);
    float scale = float(i) / 64.0f;

    // scale samples s.t. they're more aligned to center of kernel
    scale = lerp(0.1f, 1.0f, scale * scale);
    sample *= scale;
    ssaoKernel.push_back(sample);
  }

  for (unsigned int i = 0; i < 16; i++) {
    glm::vec3 noise(randomFloats(generator) * 2.0 - 1.0,
        randomFloats(generator) * 2.0 - 1.0, 0.0f);
    ssaoNoise.push_back(noise);
  }

  glGenTextures(1, &noiseTexture);
  glBindTexture(GL_TEXTURE_2D, noiseTexture);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0, GL_RGB, GL_FLOAT, &ssaoNoise[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}
