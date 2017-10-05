//
//  OBJBaker.h
//  libraries/baking/src
//
//  Created by Utkarsh Gautam on 9/29/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_OBJBaker_h
#define hifi_OBJBaker_h

#include <QtNetwork/QNetworkReply>
#include <SharedUtil.h>
#include <QtCore/QFutureSynchronizer>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>

#include "Baker.h"
#include "TextureBaker.h"
#include <FBX.h>
#include "ModelBakingLoggingCategory.h"

using TextureBakerThreadGetter = std::function<QThread*()>;

class OBJBaker : public Baker {
    Q_OBJECT

public:
    OBJBaker(const QUrl& objURL, TextureBakerThreadGetter textureThreadGetter,
             const QString& bakedOutputDir, const QString& originalOutputDir = "");
    
public slots:
    virtual void bake() override;
    virtual void abort() override;

signals:
    void OBJLoaded();

private slots:
    void startBake();
    void handleOBJNetworkReply();
    //void handleBakedTexture();
    //void handleAbortedTexture();

private:
    //void setupOutputFolder();
    void loadOBJ();
    
    QUrl _objURL;
    QString _bakedOBJFilePath;
    QString _bakedOutputDir;
    QString _originalOutputDir;
    QDir _tempDir;
    QString _originalOBJFilePath;
    
    TextureBakerThreadGetter _textureThreadGetter;
    QMultiHash<QUrl, QSharedPointer<TextureBaker>> _bakingTextures;

public:
    void createFBXNodeTree(FBXNode* objRoot, FBXNode* dracoNode);


};
#endif // !hifi_OBJBaker_h
