//
//  OBJBaker.cpp
//  libraries/baking/src
//
//  Created by Utkarsh Gautam on 9/29/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <PathUtils.h>
#include <NetworkAccessManager.h>
#include <QtConcurrent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <mutex>
#include "OBJBaker.h"
#include "ModelBakingLoggingCategory.h"
#include "TextureBaker.h"
#include "OBJReader.h"
#include "ModelBaker.h"
#include "FBXWriter.h"


qlonglong nodeID = 0;
int count = 0;
int count1 = 0;
OBJBaker::OBJBaker(const QUrl& objURL, TextureBakerThreadGetter textureThreadGetter,
                   const QString& bakedOutputDir, const QString& originalOutputDir) :
    _objURL(objURL),
    _bakedOutputDir(bakedOutputDir),
    _originalOutputDir(originalOutputDir),
    _textureThreadGetter(textureThreadGetter) {

}

void OBJBaker::abort() {
    Baker::abort();

    // tell our underlying TextureBaker instances to abort
    // the objBaker will wait until all are aborted before emitting its own abort signal
    for (auto& textureBaker : _bakingTextures) {
        textureBaker->abort();
    }
}

void OBJBaker::bake() {
    qDebug() << "OBJBaker" << _objURL << "bake starting";
    
    auto tempDir = PathUtils::generateTemporaryDir();

    if (tempDir.isEmpty()) {
        handleError("Failed to create a temporary directory.");
        return;
    }

    _tempDir = tempDir;

    _originalOBJFilePath = _tempDir.filePath(_objURL.fileName());
    qDebug() << "Made temporary dir " << _tempDir;
    qDebug() << "Origin file path: " << _originalOBJFilePath;

    connect(this, &OBJBaker::OBJLoaded, this, &OBJBaker::startBake);

    // make a local copy of the obj file
    loadOBJ();
}

void OBJBaker::loadOBJ() {
    // check if the obj is local or first needs to be downloaded
    if (_objURL.isLocalFile()) {
        // load up the local file
        QFile localobj{ _objURL.toLocalFile() };

        qDebug() << "Local file url: " << _objURL << _objURL.toString() << _objURL.toLocalFile() << ", copying to: " << _originalOBJFilePath;

        if (!localobj.exists()) {
            //QMessageBox::warning(this, "Could not find " + _objURL.toString(), "");
            handleError("Could not find " + _objURL.toString());
            return;
        }

        // make a copy in the output folder
        if (!_originalOutputDir.isEmpty()) {
            qDebug() << "Copying to: " << _originalOutputDir << "/" << _objURL.fileName();
            localobj.copy(_originalOutputDir + "/" + _objURL.fileName());
        }
                
        localobj.copy(_originalOBJFilePath);

        // emit our signal to start the import of the obj source copy
        emit OBJLoaded();
    } else {
        // remote file, kick off a download
        auto& networkAccessManager = NetworkAccessManager::getInstance();

        QNetworkRequest networkRequest;

        // setup the request to follow re-directs and always hit the network
        networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
networkRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);

networkRequest.setUrl(_objURL);

qCDebug(model_baking) << "Downloading" << _objURL;
auto networkReply = networkAccessManager.get(networkRequest);

connect(networkReply, &QNetworkReply::finished, this, &OBJBaker::handleOBJNetworkReply);
    }
}

void OBJBaker::handleOBJNetworkReply() {
    auto requestReply = qobject_cast<QNetworkReply*>(sender());

    if (requestReply->error() == QNetworkReply::NoError) {
        qCDebug(model_baking) << "Downloaded" << _objURL;

        // grab the contents of the reply and make a copy in the output folder
        QFile copyOfOriginal(_originalOBJFilePath);

        qDebug(model_baking) << "Writing copy of original obj to" << _originalOBJFilePath << copyOfOriginal.fileName();

        if (!copyOfOriginal.open(QIODevice::WriteOnly)) {
            // add an error to the error list for this obj stating that a duplicate of the original obj could not be made
            handleError("Could not create copy of " + _objURL.toString() + " (Failed to open " + _originalOBJFilePath + ")");
            return;
        }
        if (copyOfOriginal.write(requestReply->readAll()) == -1) {
            handleError("Could not create copy of " + _objURL.toString() + " (Failed to write)");
            return;
        }

        // close that file now that we are done writing to it
        copyOfOriginal.close();

        if (!_originalOutputDir.isEmpty()) {
            copyOfOriginal.copy(_originalOutputDir + "/" + _objURL.fileName());
        }

        // emit our signal to start the import of the obj source copy
        emit OBJLoaded();
    } else {
        // add an error to our list stating that the obj could not be downloaded
        handleError("Failed to download " + _objURL.toString());
    }
}


void OBJBaker::startBake() {
    // Read the OBJ
    QFile objFile(_originalOBJFilePath);
    if (!objFile.open(QIODevice::ReadOnly)) {
        handleError("Error opening " + _originalOBJFilePath + " for reading");
        return;
    }

    QByteArray objData = objFile.readAll();

    bool combineParts = true;
    OBJReader reader;
    FBXGeometry* geometry = reader.readOBJ(objData, QVariantHash(), combineParts, _objURL);
    
    // Write OBJ Data in form of FBX tree nodes
    FBXNode objRoot;
    FBXNode dracoNode;
    createFBXNodeTree(&objRoot, geometry);

    // Serialize the resultant FBX tree
    auto encodedFBX = FBXWriter::encodeFBX(objRoot);

    // Export as baked FBX
    auto fileName = _objURL.fileName();
    auto baseName = fileName.left(fileName.lastIndexOf('.'));
    auto bakedFilename = baseName + ".baked.fbx";

    _bakedOBJFilePath = _bakedOutputDir + "/" + bakedFilename;

    QFile bakedFile;
    bakedFile.setFileName(_bakedOBJFilePath);
    if (!bakedFile.open(QIODevice::WriteOnly)) {
        handleError("Error opening " + _bakedOBJFilePath + " for writing");
        return;
    }

    bakedFile.write(encodedFBX);

    // Export successful
    _outputFiles.push_back(_bakedOBJFilePath);
    qCDebug(model_baking) << "Exported" << _objURL << "to" << _bakedOBJFilePath;
    
    emit finished();

}

void OBJBaker::createFBXNodeTree(FBXNode * objRoot, FBXGeometry * geometry) {
    // FBX Header
    FBXNode headerNode;
    headerNode.name = "FBXHeaderExtension";

    // Generating global settings node
    // Required for Unit Scale Factor
    FBXNode globalSettingsNode;
    globalSettingsNode.name = "GlobalSettings";
    FBXNode properties70Node;
    properties70Node.name = "Properties70";
    FBXNode pNode;
    pNode.name = "P";
    setProperties(&pNode);
    properties70Node.children = { pNode };
    globalSettingsNode.children = { properties70Node };

    // Generating Object node
    objectNode.name = "Objects";

    // Generating Object node's child - Geometry node 
    FBXNode geometryNode;
    geometryNode.name = "Geometry";
    setProperties(&geometryNode);
    
    // Get the compressed mesh in dracoNode
    /*ExtractedMesh extractedMesh;
    extractedMesh.mesh = geometry->meshes[0];
    for (int i = 0;i < geometry->meshes[0].parts.size();i++) {
        extractedMesh.partMaterialTextures.push_back(QPair<int, int>(i, i));
    }*/
    
    bool hasDeformers = false;

    //FBXNode* dracoNode = _baker.compressMesh(extractedMesh, hasDeformers);
    FBXNode* dracoNode = _baker.compressMesh(geometry->meshes[0], hasDeformers);
    geometryNode.children.append(*dracoNode);

    // Generating Object node's child - Model node
    FBXNode modelNode;
    modelNode.name = "Model";
    setProperties(&modelNode);
    objectNode.children = { geometryNode, modelNode };

    // Generating Objects node's child - Material node

    // Remove duplicate material information
    const QString SMART_DEFAULT_MATERIAL_NAME = "High Fidelity smart default material name";
    geometry->materials.erase(geometry->materials.find(SMART_DEFAULT_MATERIAL_NAME));
    
    QVector<FBXMeshPart> meshParts = geometry->meshes[0].parts;
    for (auto p : meshParts) {
        FBXNode materialNode;
        materialNode.name = "Material";
        setMaterialNodeProperties(&materialNode, p.materialID, geometry);
        objectNode.children.append(materialNode);
    }
    
    // Texture Node
    for (auto p : meshParts) {
        QString material = p.materialID;
        FBXMaterial currentMaterial = geometry->materials[material];
        if (!currentMaterial.albedoTexture.filename.isEmpty() || !currentMaterial.specularTexture.filename.isEmpty()) {
            _textureID = nodeID;
            QVariant property0(nodeID++);
            FBXNode textureNode;
            textureNode.name = "Texture";
            textureNode.properties = { property0 };

            FBXNode textureNameNode;
            textureNameNode.name = "TextureName";
            QByteArray x = "Ka";
            auto prop0 = QVariant::fromValue(QByteArray(x.data(), (int)x.size()));
            textureNameNode.properties = { prop0 };

            FBXNode relativeFilenameNode;
            relativeFilenameNode.name = "RelativeFilename";
            QByteArray s = currentMaterial.albedoTexture.filename;
            QByteArray* textureFile = _baker.compressTexture(s, _objURL, _bakedOutputDir, _textureThreadGetter, _originalOutputDir);
            QVariant textureProperty0;
            textureProperty0 = QVariant::fromValue(QByteArray(textureFile->data(), (int)textureFile->size()));
            relativeFilenameNode.properties = { textureProperty0 };

            FBXNode properties70Node;
            properties70Node.name = "Properties70";

            QVariant texProperty0;
            QVariant texProperty1;
            QVariant texProperty2;
            QVariant texProperty3;
            QVariant texProperty4;

            double value;

            // Set UseMaterial
            FBXNode pUseMaterial;
            pUseMaterial.name = "P";
            s = "UseMaterial";
            texProperty0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "bool";
            texProperty1 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty2 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty3 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            
            int texVal = 1;
            texProperty4 = texVal;

            pUseMaterial.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };

            FBXNode pUVSet;
            pUVSet.name = "P";
            s = "UVSet";
            texProperty0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "KString";
            texProperty1 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty2 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty3 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty4 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));

            pUVSet.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };

            FBXNode pUseMipMap;
            pUseMipMap.name = "P";
            s = "UseMipMap";
            texProperty0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "bool";
            texProperty1 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty2 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
            s = "";
            texProperty3 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));

            texVal = 1;
            texProperty4 = texVal;

            pUseMipMap.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };

            properties70Node.children = { pUVSet, pUseMaterial, pUseMipMap };

            textureNode.children = { textureNameNode,relativeFilenameNode, properties70Node };

            objectNode.children.append(textureNode);
        }
    }
    
    // Another texture Node
    
    //_textureID1 = nodeID;
    //QVariant property01(nodeID++);
    //FBXNode textureNode;
    //textureNode.name = "Texture";
    //textureNode.properties = { property01 };

    //FBXNode textureNameNode;
    //textureNameNode.name = "TextureName";
    //QByteArray tempName = "Kd";
    //auto prop0 = QVariant::fromValue(QByteArray(tempName.data(), (int)tempName.size()));
    //textureNameNode.properties = { prop0 };

    //FBXNode relativeFilenameNode;
    //relativeFilenameNode.name = "RelativeFilename";
    //QByteArray s2 = "canvas.png";
    //QByteArray textureFile = _baker.compressTexture(s2, _objURL, _bakedOutputDir, _textureThreadGetter, _originalOutputDir);
    //QVariant textureProperty0;
    //textureProperty0 = QVariant::fromValue(QByteArray(textureFile.data(), (int)textureFile.size()));
    //relativeFilenameNode.properties = { textureProperty0 };

    //FBXNode properties70Node1;
    //properties70Node1.name = "Properties70";

    //QVariant texProperty0;
    //QVariant texProperty1;
    //QVariant texProperty2;
    //QVariant texProperty3;
    //QVariant texProperty4;

    //// Set UseMaterial
    //
    //FBXNode pUVSet;
    //pUVSet.name = "P";
    //s2 = "UVSet";
    //texProperty0 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "KString";
    //texProperty1 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty2 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty3 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty4 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));

    //pUVSet.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };

    //FBXNode pUseMaterial;
    //pUseMaterial.name = "P";
    //s2 = "UseMaterial";
    //texProperty0 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "bool";
    //texProperty1 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty2 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty3 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));

    //int texVal = 1;
    //texProperty4 = texVal;

    //pUseMaterial.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };
    //

    //FBXNode pUseMipMap;
    //pUseMipMap.name = "P";
    //s2 = "UseMipMap";
    //texProperty0 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "bool";
    //texProperty1 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty2 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));
    //s2 = "";
    //texProperty3 = QVariant::fromValue(QByteArray(s2.data(), (int)s2.size()));

    //texVal = 1;
    //texProperty4 = texVal;

    //pUseMipMap.properties = { texProperty0, texProperty1, texProperty2, texProperty3, texProperty4 };

    //
    //properties70Node1.children = { pUVSet, pUseMaterial, pUseMipMap };

    //textureNode.children = { textureNameNode,relativeFilenameNode, properties70Node1 };

    //objectNode.children.append(textureNode);

    //// Generate Video Node
    //FBXNode videoNode;
    //videoNode.name = "Video";
    //_videoID = nodeID;
    //QVariant videoProperty0(nodeID++);

    //FBXNode relativeFileNameNode;
    //relativeFileNameNode.name = "RelativeFilename";
    //QByteArray s1 = "canvas.png";
    //QVariant videoChildProperty0;
    //videoChildProperty0 = QVariant::fromValue(QByteArray(s1.data(), (int)s1.size()));
    //relativeFileNameNode.properties = { videoChildProperty0 };
    //videoNode.properties = { videoProperty0 };
    //videoNode.children = { relativeFileNameNode };
    //objectNode.children.append(videoNode);

    
    // Generating Connections node
    FBXNode connectionsNode;
    connectionsNode.name = "Connections";
    // connect Geometry -> Model 
    FBXNode cNode1;
    cNode1.name = "C";
    QByteArray propertyString("OO");
    QVariant property0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    qlonglong childID = _geometryID;
    QVariant property1(childID);
    qlonglong parentID = _modelID;
    QVariant property2(parentID);
    cNode1.properties = { property0, property1, property2 };
    connectionsNode.children = { cNode1};

    // connect materials to model
    for (int i = 0;i < geometry->materials.size();i++) {
        FBXNode cNode;
        cNode.name = "C";
        property0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        property1 = _materialIDs[i];
        property2 = _modelID;
        cNode.properties = { property0, property1, property2 };
        connectionsNode.children.append(cNode);
    }
    // Texture to material
    //FBXNode cNode2;
    //cNode2.name = "C";
    //property0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
    //property1 = _textureID;
    //property2 = _materialIDs[1];
    //cNode2.properties = { property0, property1, property2 };
    //connectionsNode.children.append(cNode2);

    //FBXNode cNode4;
    //cNode4.name = "C";
    //property0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
    //property1 = _textureID1;
    //property2 = _materialIDs[1];
    //cNode4.properties = { property0, property1, property2 };
    //connectionsNode.children.append(cNode4);

    //// Video to texture
    //FBXNode cNode3;
    //cNode3.name = "C";
    //property0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
    //property1 = _videoID;
    //property2 = _textureID;
    //cNode3.properties = { property0, property1, property2 };
    //connectionsNode.children.append(cNode3);

    //FBXNode cNode5;
    //cNode5.name = "C";
    //property0 = QVariant::fromValue(QByteArray(s.data(), (int)s.size()));
    //property1 = _videoID;
    //property2 = _textureID1;
    //cNode5.properties = { property0, property1, property2 };
    //connectionsNode.children.append(cNode5);
    
    objRoot->children = { globalSettingsNode, objectNode, connectionsNode };
}

void OBJBaker::setProperties(FBXNode* parentNode) {
    if (parentNode->name == "P") {
        QByteArray propertyString("UnitScaleFactor");
        QVariant property0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        propertyString = "double";
        QVariant property1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        propertyString = "Number";
        QVariant property2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        propertyString = "";
        QVariant property3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        double unitScaleFactor = 100;
        QVariant property4(unitScaleFactor);

        parentNode->properties = { property0, property1, property2, property3, property4 };
    } else if (parentNode->name == "Geometry") {
        _geometryID = nodeID;
        QVariant property0(nodeID++);
        QByteArray propertyString("Geometry");
        QVariant property1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        propertyString = "Mesh";
        QVariant property2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));

        parentNode->properties = { property0, property1, property2 };
    } else if (parentNode->name == "Model") {
        _modelID = nodeID;
        QVariant property0(nodeID++);
        QByteArray propertyString("Model");
        QVariant property1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
        propertyString = "Mesh";
        QVariant property2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));

        parentNode->properties = { property0, property1, property2 };
    }
}

void OBJBaker::setMaterialNodeProperties(FBXNode* materialNode, QString material, FBXGeometry* geometry) {
    // Set materialNode properties
    _materialIDs.push_back(nodeID);
    QVariant property0(nodeID++);
    QByteArray propertyString(material.toLatin1());
    QVariant property1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Mesh";
    QVariant property2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));

    materialNode->properties = { property0, property1, property2 };
    
    FBXMaterial currentMaterial = geometry->materials[material];

    FBXNode properties70Node;
    properties70Node.name = "Properties70";

    QVariant materialProperty0;
    QVariant materialProperty1;
    QVariant materialProperty2;
    QVariant materialProperty3;
    QVariant materialProperty4;
    QVariant materialProperty5;
    QVariant materialProperty6;

    double value;

    // Set diffuseColor
    FBXNode pNodeDiffuseColor;
    pNodeDiffuseColor.name = "P";
    propertyString = "DiffuseColor";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Color";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)currentMaterial.diffuseColor[0];
    materialProperty4 = value;
    value = (double)currentMaterial.diffuseColor[1];
    materialProperty5 = value;
    value = (double)currentMaterial.diffuseColor[2];
    materialProperty6 = value;

    pNodeDiffuseColor.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4, materialProperty5, materialProperty6 };
    properties70Node.children.append(pNodeDiffuseColor);

    // Set specularColor
    FBXNode pNodeSpecularColor;
    pNodeSpecularColor.name = "P";
    propertyString = "SpecularColor";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Color";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)currentMaterial.specularColor[0];
    materialProperty4 = value;
    value = (double)currentMaterial.specularColor[1];
    materialProperty5 = value;
    value = (double)currentMaterial.specularColor[2];
    materialProperty6 = value;

    pNodeSpecularColor.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4, materialProperty5, materialProperty6 };
    properties70Node.children.append(pNodeSpecularColor);

    // Set Shininess

    FBXNode pNodeShininess;
    pNodeShininess.name = "P";
    propertyString = "Shininess";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Number";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)currentMaterial.shininess;
    materialProperty4 = value;

    pNodeShininess.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4 };
    properties70Node.children.append(pNodeShininess);

    // Set Opacity

    FBXNode pNodeOpacity;
    pNodeOpacity.name = "P";
    propertyString = "Opacity";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Number";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)currentMaterial.opacity;
    materialProperty4 = value;

    pNodeOpacity.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4 };
    properties70Node.children.append(pNodeOpacity);

    // Set DiffuseFactor
    FBXNode pNodeDiffusefactor;
    pNodeDiffusefactor.name = "P";
    propertyString = "DiffuseFactor";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Number";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)1;
    materialProperty4 = value;

    pNodeDiffusefactor.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4 };
    properties70Node.children.append(pNodeDiffusefactor);

    // Set SpecularFactor
    FBXNode pNodeSpecularfactor;
    pNodeSpecularfactor.name = "P";
    propertyString = "SpecularFactor";
    materialProperty0 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "Number";
    materialProperty1 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "";
    materialProperty2 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    propertyString = "A";
    materialProperty3 = QVariant::fromValue(QByteArray(propertyString.data(), (int)propertyString.size()));
    value = (double)1;
    materialProperty4 = value;

    pNodeSpecularfactor.properties = { materialProperty0, materialProperty1, materialProperty2, materialProperty3, materialProperty4 };
    properties70Node.children.append(pNodeSpecularfactor);
    
    materialNode->children.append(properties70Node);
}
