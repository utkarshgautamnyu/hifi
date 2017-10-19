#include "ModelBaker.h"

#include <image\Image.h>

#include <QtConcurrent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <mutex>

#include <NetworkAccessManager.h>
#include <SharedUtil.h>

#include <PathUtils.h>

#include <FBXReader.h>
#include <FBXWriter.h>

#include "ModelBakingLoggingCategory.h"
#include "TextureBaker.h"

#include "FBXBaker.h"

#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable : 4267 )
#endif

#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <draco/compression/encode.h>

#ifdef _WIN32
#pragma warning( pop )
#endif

ModelBaker::ModelBaker() {}

void ModelBaker::abort() {
    Baker::abort();

    // tell our underlying TextureBaker instances to abort
    // the FBXBaker will wait until all are aborted before emitting its own abort signal
    for (auto& textureBaker : _bakingTextures) {
        textureBaker->abort();
    }
}

void ModelBaker::bake() {}

FBXNode* ModelBaker::compressMesh(FBXMesh& mesh, bool hasDeformers, getMaterialIDCallback callback) {
    //auto& mesh = extractedMesh.mesh;
    //bool hasDeformers = false;
    if (mesh.wasCompressed) {
        handleError("Cannot re-bake a file that contains compressed mesh");
        return nullptr;
    }

    Q_ASSERT(mesh.normals.size() == 0 || mesh.normals.size() == mesh.vertices.size());
    Q_ASSERT(mesh.colors.size() == 0 || mesh.colors.size() == mesh.vertices.size());
    Q_ASSERT(mesh.texCoords.size() == 0 || mesh.texCoords.size() == mesh.vertices.size());

    int64_t numTriangles{ 0 };
    for (auto& part : mesh.parts) {
        if ((part.quadTrianglesIndices.size() % 3) != 0 || (part.triangleIndices.size() % 3) != 0) {
            handleWarning("Found a mesh part with invalid index data, skipping");
            continue;
        }
        numTriangles += part.quadTrianglesIndices.size() / 3;
        numTriangles += part.triangleIndices.size() / 3;
    }

    if (numTriangles == 0) {
        //continue;
        return nullptr;
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
    uint16_t materialID;
    
    for (auto& part : mesh.parts) {
        if (callback) {
            materialID = callback(partIndex);
        } else {
            materialID = partIndex;
        }
        
        auto addFace = [&](QVector<int>& indices, int index, draco::FaceIndex face) {
            int32_t idx0 = indices[index];
            int32_t idx1 = indices[index + 1];
            int32_t idx2 = indices[index + 2];

            if (hasPerFaceMaterials) {
                meshBuilder.SetPerFaceAttributeValueForFace(faceMaterialAttributeID, face, &materialID);
            }

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
        handleWarning("Failed to finalize the baking of a draco Geometry node");
        //continue;
        return nullptr;
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

    static FBXNode dracoMeshNode;
    dracoMeshNode.name = "DracoMesh";
    auto value = QVariant::fromValue(QByteArray(buffer.data(), (int)buffer.size()));
    dracoMeshNode.properties.append(value);

    return &dracoMeshNode;
}

QByteArray* ModelBaker::compressTexture(QString modelTextureFileName, QUrl modelURL, QString bakedOutputDir, TextureBakerThreadGetter textureThreadGetter, 
                                        getTextureContentTypeCallback textureContentTypeCallback, const QString& originalOutputDir) {
    _modelURL = modelURL;
    _textureThreadGetter = textureThreadGetter;
    _originalOutputDir = originalOutputDir;
    qCDebug(model_baking) << "ReachedHEre";
    static QByteArray textureChild;
    
    QPair<QByteArray, image::TextureUsage::Type> textureContentType;
    
    if (textureContentTypeCallback) {
        textureContentType = textureContentTypeCallback();
    } else {
        textureContentType = QPair<QByteArray, image::TextureUsage::Type>(NULL, image::TextureUsage::Type::OCCLUSION_TEXTURE);
    }
    
    QByteArray textureContent = textureContentType.first;
    image::TextureUsage::Type textureType = textureContentType.second;

    QFileInfo modelTextureFileInfo{ modelTextureFileName.replace("\\", "/") };
    
    if (modelTextureFileInfo.suffix() == BAKED_TEXTURE_EXT.mid(1)) {
        // re-baking a model that already references baked textures
        // this is an error - return from here
        handleError("Cannot re-bake a file that already references compressed textures");
        return nullptr;
    }

    // make sure this texture points to something and isn't one we've already re-mapped
    if (!modelTextureFileInfo.filePath().isEmpty()) {
        // check if this was an embedded texture that we already have in-memory content for
        
        // figure out the URL to this texture, embedded or external
        auto urlToTexture = getTextureURL(modelTextureFileInfo, modelTextureFileName, !textureContent.isNull());

        QString bakedTextureFileName;
        if (_remappedTexturePaths.contains(urlToTexture)) {
            bakedTextureFileName = _remappedTexturePaths[urlToTexture];
        } else {
            // construct the new baked texture file name and file path
            // ensuring that the baked texture will have a unique name
            // even if there was another texture with the same name at a different path
            bakedTextureFileName = createBakedTextureFileName(modelTextureFileInfo);
            _remappedTexturePaths[urlToTexture] = bakedTextureFileName;
        }

        qCDebug(model_baking).noquote() << "Re-mapping" << modelTextureFileName
            << "to" << bakedTextureFileName;

        QString bakedTextureFilePath{
            bakedOutputDir + "/" + bakedTextureFileName
        };

        // write the new filename into the FBX scene
        textureChild = bakedTextureFileName.toLocal8Bit();

        if (!_bakingTextures.contains(urlToTexture)) {
            _outputFiles.push_back(bakedTextureFilePath);
            qCDebug(model_baking) << "BakedTextureFilePath" << bakedTextureFilePath;
            // grab the ID for this texture so we can figure out the
            // texture type from the loaded materials
            //QString textureID{ object->properties[0].toByteArray() };
            //auto textureType = textureTypes[textureID];
            //auto textureType = image::TextureUsage::Type::OCCLUSION_TEXTURE;

            // bake this texture asynchronously
            bakeTexture(urlToTexture, textureType, bakedOutputDir, bakedTextureFileName, textureContent);
        }
    }
   
    return &textureChild;
}

QUrl ModelBaker::getTextureURL(const QFileInfo& textureFileInfo, QString relativeFileName, bool isEmbedded) {

    QUrl urlToTexture;

    auto apparentRelativePath = QFileInfo(relativeFileName.replace("\\", "/"));

    if (isEmbedded) {
        urlToTexture = _modelURL.toString() + "/" + apparentRelativePath.filePath();
    } else {
        if (textureFileInfo.exists() && textureFileInfo.isFile()) {
            // set the texture URL to the local texture that we have confirmed exists
            urlToTexture = QUrl::fromLocalFile(textureFileInfo.absoluteFilePath());
        } else {
            // external texture that we'll need to download or find

            // this is a relative file path which will require different handling
            // depending on the location of the original FBX
            if (_modelURL.isLocalFile() && apparentRelativePath.exists() && apparentRelativePath.isFile()) {
                // the absolute path we ran into for the texture in the FBX exists on this machine
                // so use that file
                urlToTexture = QUrl::fromLocalFile(apparentRelativePath.absoluteFilePath());
            } else {
                // we didn't find the texture on this machine at the absolute path
                // so assume that it is right beside the FBX to match the behaviour of interface
                urlToTexture = _modelURL.resolved(apparentRelativePath.fileName());
            }
        }
    }

    return urlToTexture;
}

void ModelBaker::bakeTexture(const QUrl& textureURL, image::TextureUsage::Type textureType,
                           const QDir& outputDir, const QString& bakedFilename, const QByteArray& textureContent) {
    
    // start a bake for this texture and add it to our list to keep track of
    QSharedPointer<TextureBaker> bakingTexture{
        new TextureBaker(textureURL, textureType, outputDir, bakedFilename, textureContent),
        &TextureBaker::deleteLater
    };
    
    // make sure we hear when the baking texture is done or aborted
    connect(bakingTexture.data(), &Baker::finished, this, &ModelBaker::handleBakedTexture);
    connect(bakingTexture.data(), &TextureBaker::aborted, this, &ModelBaker::handleAbortedTexture);

    // keep a shared pointer to the baking texture
    _bakingTextures.insert(textureURL, bakingTexture);

    // start baking the texture on one of our available worker threads
    bakingTexture->moveToThread(_textureThreadGetter());
    QMetaObject::invokeMethod(bakingTexture.data(), "bake");
}

void ModelBaker::handleBakedTexture() {
    TextureBaker* bakedTexture = qobject_cast<TextureBaker*>(sender());

    // make sure we haven't already run into errors, and that this is a valid texture
    if (bakedTexture) {
        if (!shouldStop()) {
            if (!bakedTexture->hasErrors()) {
                if (!_originalOutputDir.isEmpty()) {
                    // we've been asked to make copies of the originals, so we need to make copies of this if it is a linked texture

                    // use the path to the texture being baked to determine if this was an embedded or a linked texture

                    // it is embeddded if the texure being baked was inside a folder with the name of the FBX
                    // since that is the fake URL we provide when baking external textures

                    if (!_modelURL.isParentOf(bakedTexture->getTextureURL())) {
                        // for linked textures we want to save a copy of original texture beside the original FBX

                        qCDebug(model_baking) << "Saving original texture for" << bakedTexture->getTextureURL();

                        // check if we have a relative path to use for the texture
                        auto relativeTexturePath = texturePathRelativeToModel(_modelURL, bakedTexture->getTextureURL());

                        QFile originalTextureFile{
                            _originalOutputDir + "/" + relativeTexturePath + bakedTexture->getTextureURL().fileName()
                        };

                        if (relativeTexturePath.length() > 0) {
                            // make the folders needed by the relative path
                        }

                        if (originalTextureFile.open(QIODevice::WriteOnly) && originalTextureFile.write(bakedTexture->getOriginalTexture()) != -1) {
                            qCDebug(model_baking) << "Saved original texture file" << originalTextureFile.fileName()
                                << "for" << _modelURL;
                        } else {
                            handleError("Could not save original external texture " + originalTextureFile.fileName()
                                        + " for " + _modelURL.toString());
                            return;
                        }
                    }
                }


                // now that this texture has been baked and handled, we can remove that TextureBaker from our hash
                _bakingTextures.remove(bakedTexture->getTextureURL());

                checkIfTexturesFinished();
            } else {
                // there was an error baking this texture - add it to our list of errors
                _errorList.append(bakedTexture->getErrors());

                // we don't emit finished yet so that the other textures can finish baking first
                _pendingErrorEmission = true;

                // now that this texture has been baked, even though it failed, we can remove that TextureBaker from our list
                _bakingTextures.remove(bakedTexture->getTextureURL());

                // abort any other ongoing texture bakes since we know we'll end up failing
                for (auto& bakingTexture : _bakingTextures) {
                    bakingTexture->abort();
                }

                checkIfTexturesFinished();
            }
        } else {
            // we have errors to attend to, so we don't do extra processing for this texture
            // but we do need to remove that TextureBaker from our list
            // and then check if we're done with all textures
            _bakingTextures.remove(bakedTexture->getTextureURL());

            checkIfTexturesFinished();
        }
    }
}

QString ModelBaker::texturePathRelativeToModel(QUrl fbxURL, QUrl textureURL) {
    auto fbxPath = fbxURL.toString(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment);
    auto texturePath = textureURL.toString(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment);

    if (texturePath.startsWith(fbxPath)) {
        // texture path is a child of the FBX path, return the texture path without the fbx path
        return texturePath.mid(fbxPath.length());
    } else {
        // the texture path was not a child of the FBX path, return the empty string
        return "";
    }
}

void ModelBaker::checkIfTexturesFinished() {
    // check if we're done everything we need to do for this FBX
    // and emit our finished signal if we're done

    if (_bakingTextures.isEmpty()) {
        if (shouldStop()) {
            // if we're checking for completion but we have errors
            // that means one or more of our texture baking operations failed

            if (_pendingErrorEmission) {
                setIsFinished(true);
            }

            return;
        } else {
            qCDebug(model_baking) << "Finished baking, emitting finished" << _modelURL;

            setIsFinished(true);
        }
    }
}

void ModelBaker::handleAbortedTexture() {
    // grab the texture bake that was aborted and remove it from our hash since we don't need to track it anymore
    TextureBaker* bakedTexture = qobject_cast<TextureBaker*>(sender());

    if (bakedTexture) {
        _bakingTextures.remove(bakedTexture->getTextureURL());
    }

    // since a texture we were baking aborted, our status is also aborted
    _shouldAbort.store(true);

    // abort any other ongoing texture bakes since we know we'll end up failing
    for (auto& bakingTexture : _bakingTextures) {
        bakingTexture->abort();
    }

    checkIfTexturesFinished();
}

QString ModelBaker::createBakedTextureFileName(const QFileInfo& textureFileInfo) {
    // first make sure we have a unique base name for this texture
    // in case another texture referenced by this model has the same base name
    auto& nameMatches = _textureNameMatchCount[textureFileInfo.baseName()];

    QString bakedTextureFileName{ textureFileInfo.completeBaseName() };

    if (nameMatches > 0) {
        // there are already nameMatches texture with this name
        // append - and that number to our baked texture file name so that it is unique
        bakedTextureFileName += "-" + QString::number(nameMatches);
    }

    bakedTextureFileName += BAKED_TEXTURE_EXT;

    // increment the number of name matches
    ++nameMatches;

    return bakedTextureFileName;
}

void ModelBaker::setWasAborted(bool wasAborted) {
    if (wasAborted != _wasAborted.load()) {
        Baker::setWasAborted(wasAborted);

        if (wasAborted) {
            qCDebug(model_baking) << "Aborted baking" << _modelURL;
        }
    }
}
