#include "ModelBaker.h"

#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable : 4267 )
#endif

#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <draco/compression/encode.h>

#ifdef _WIN32
#pragma warning( pop )
#endif

#include <fstream>
#include <iostream>
ModelBaker::ModelBaker() {}

FBXNode ModelBaker::compressMesh(FBXMesh& mesh) {
    
    FBXNode x;

    bool hasDeformers = false;
    if (mesh.wasCompressed) {
        //handleError("Cannot re-bake a file that contains compressed mesh");
        return x;
    }

    Q_ASSERT(mesh.normals.size() == 0 || mesh.normals.size() == mesh.vertices.size());
    Q_ASSERT(mesh.colors.size() == 0 || mesh.colors.size() == mesh.vertices.size());
    Q_ASSERT(mesh.texCoords.size() == 0 || mesh.texCoords.size() == mesh.vertices.size());

    int64_t numTriangles{ 0 };
    for (auto& part : mesh.parts) {
        if ((part.quadTrianglesIndices.size() % 3) != 0 || (part.triangleIndices.size() % 3) != 0) {
            //handleWarning("Found a mesh part with invalid index data, skipping");
            continue;
        }
        numTriangles += part.quadTrianglesIndices.size() / 3;
        numTriangles += part.triangleIndices.size() / 3;
    }

    if (numTriangles == 0) {
        //continue;
        return x;
    }

    draco::TriangleSoupMeshBuilder meshBuilder;

    meshBuilder.Start(numTriangles);

    bool hasNormals{ mesh.normals.size() > 0 };
    bool hasColors{ mesh.colors.size() > 0 };
    bool hasTexCoords{ mesh.texCoords.size() > 0 };
    bool hasTexCoords1{ mesh.texCoords1.size() > 0 };
    bool hasPerFaceMaterials{ mesh.parts.size() > 1 };
    bool needsOriginalIndices{ hasDeformers };

    int normalsAttributeID{ -1 };
    int colorsAttributeID{ -1 };
    int texCoordsAttributeID{ -1 };
    int texCoords1AttributeID{ -1 };
    int faceMaterialAttributeID{ -1 };
    int originalIndexAttributeID{ -1 };

    const int positionAttributeID = meshBuilder.AddAttribute(draco::GeometryAttribute::POSITION,
                                                             3, draco::DT_FLOAT32);
    if (needsOriginalIndices) {
        originalIndexAttributeID = meshBuilder.AddAttribute(
            (draco::GeometryAttribute::Type)DRACO_ATTRIBUTE_ORIGINAL_INDEX,
            1, draco::DT_INT32);
    }

    if (hasNormals) {
        normalsAttributeID = meshBuilder.AddAttribute(draco::GeometryAttribute::NORMAL,
                                                      3, draco::DT_FLOAT32);
    }
    if (hasColors) {
        colorsAttributeID = meshBuilder.AddAttribute(draco::GeometryAttribute::COLOR,
                                                     3, draco::DT_FLOAT32);
    }
    if (hasTexCoords) {
        texCoordsAttributeID = meshBuilder.AddAttribute(draco::GeometryAttribute::TEX_COORD,
                                                        2, draco::DT_FLOAT32);
    }
    if (hasTexCoords1) {
        texCoords1AttributeID = meshBuilder.AddAttribute(
            (draco::GeometryAttribute::Type)DRACO_ATTRIBUTE_TEX_COORD_1,
            2, draco::DT_FLOAT32);
    }
    if (hasPerFaceMaterials) {
        faceMaterialAttributeID = meshBuilder.AddAttribute(
            (draco::GeometryAttribute::Type)DRACO_ATTRIBUTE_MATERIAL_ID,
            1, draco::DT_UINT16);
    }


    auto partIndex = 0;
    draco::FaceIndex face;
    for (auto& part : mesh.parts) {
        //const auto& matTex = extractedMesh.partMaterialTextures[partIndex];
        //uint16_t materialID = matTex.first;

        auto addFace = [&](QVector<int>& indices, int index, draco::FaceIndex face) {
            int32_t idx0 = indices[index];
            int32_t idx1 = indices[index + 1];
            int32_t idx2 = indices[index + 2];

           /* if (hasPerFaceMaterials) {
                meshBuilder.SetPerFaceAttributeValueForFace(faceMaterialAttributeID, face, &materialID);
            }*/

            meshBuilder.SetAttributeValuesForFace(positionAttributeID, face,
                                                  &mesh.vertices[idx0], &mesh.vertices[idx1],
                                                  &mesh.vertices[idx2]);

            if (needsOriginalIndices) {
                meshBuilder.SetAttributeValuesForFace(originalIndexAttributeID, face,
                                                      &mesh.originalIndices[idx0],
                                                      &mesh.originalIndices[idx1],
                                                      &mesh.originalIndices[idx2]);
            }
            if (hasNormals) {
                meshBuilder.SetAttributeValuesForFace(normalsAttributeID, face,
                                                      &mesh.normals[idx0], &mesh.normals[idx1],
                                                      &mesh.normals[idx2]);
            }
            if (hasColors) {
                meshBuilder.SetAttributeValuesForFace(colorsAttributeID, face,
                                                      &mesh.colors[idx0], &mesh.colors[idx1],
                                                      &mesh.colors[idx2]);
            }
            if (hasTexCoords) {
                meshBuilder.SetAttributeValuesForFace(texCoordsAttributeID, face,
                                                      &mesh.texCoords[idx0], &mesh.texCoords[idx1],
                                                      &mesh.texCoords[idx2]);
            }
            if (hasTexCoords1) {
                meshBuilder.SetAttributeValuesForFace(texCoords1AttributeID, face,
                                                      &mesh.texCoords1[idx0], &mesh.texCoords1[idx1],
                                                      &mesh.texCoords1[idx2]);
            }
        };

        for (int i = 0; (i + 2) < part.quadTrianglesIndices.size(); i += 3) {
            addFace(part.quadTrianglesIndices, i, face++);
        }

        for (int i = 0; (i + 2) < part.triangleIndices.size(); i += 3) {
            addFace(part.triangleIndices, i, face++);
        }

        partIndex++;
    }

    auto dracoMesh = meshBuilder.Finalize();

    if (!dracoMesh) {
        //handleWarning("Failed to finalize the baking of a draco Geometry node");
        //continue;
        return x;
    }

    // we need to modify unique attribute IDs for custom attributes
    // so the attributes are easily retrievable on the other side
    if (hasPerFaceMaterials) {
        dracoMesh->attribute(faceMaterialAttributeID)->set_unique_id(DRACO_ATTRIBUTE_MATERIAL_ID);
    }

    if (hasTexCoords1) {
        dracoMesh->attribute(texCoords1AttributeID)->set_unique_id(DRACO_ATTRIBUTE_TEX_COORD_1);
    }

    if (needsOriginalIndices) {
        dracoMesh->attribute(originalIndexAttributeID)->set_unique_id(DRACO_ATTRIBUTE_ORIGINAL_INDEX);
    }

    draco::Encoder encoder;

    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 12);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 10);
    encoder.SetSpeedOptions(0, 5);

    draco::EncoderBuffer buffer;
    encoder.EncodeMeshToBuffer(*dracoMesh, &buffer);

    FBXNode dracoMeshNode;
    dracoMeshNode.name = "DracoMesh";
    auto value = QVariant::fromValue(QByteArray(buffer.data(), (int)buffer.size()));
    dracoMeshNode.properties.append(value);

    const std::string &file_name1 = "C:/Users/utkarsh/Desktop/result.drc";
    std::ofstream out_file(file_name1, std::ios::binary);
    out_file.write(buffer.data(), (int)buffer.size());

    //objectChild.children.push_back(dracoMeshNode);

    static const std::vector<QString> nodeNamesToDelete{
        // Node data that is packed into the draco mesh
        "Vertices",
        "PolygonVertexIndex",
        "LayerElementNormal",
        "LayerElementColor",
        "LayerElementUV",
        "LayerElementMaterial",
        "LayerElementTexture",

        // Node data that we don't support
        "Edges",
        "LayerElementTangent",
        "LayerElementBinormal",
        "LayerElementSmoothing"
    };
    /*auto& children = objectChild.children;
    auto it = children.begin();
    while (it != children.end()) {
        auto begin = nodeNamesToDelete.begin();
        auto end = nodeNamesToDelete.end();
        if (find(begin, end, it->name) != end) {
            it = children.erase(it);
        } else {
            ++it;
        }
    }*/

    return dracoMeshNode;
}