#ifndef hifi_ModelBaker_h
#define hifi_ModelBaker_h

#include <FBX.h>
#include "Baker.h"

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

#include <gpu/Texture.h> 

using TextureBakerThreadGetter = std::function<QThread*()>;

class ModelBaker : public Baker {
    Q_OBJECT

public slots:
    virtual void bake() override;

public:
    ModelBaker();
    FBXNode compressMesh(ExtractedMesh& extractedMesh, bool hasDeformers);
    QByteArray compressTexture(QString textureFileName, QUrl modelUrl, QString bakedOutputDir, TextureBakerThreadGetter textureThreadGetter, const QString& originalOutputDir = "");
    QString createBakedTextureFileName(const QFileInfo & textureFileInfo);
private:
    QHash<QString, int> _textureNameMatchCount;
    QHash<QUrl, QString> _remappedTexturePaths;
    QUrl getTextureURL(const QFileInfo& textureFileInfo, QString relativeFileName, bool isEmbedded = false);
    void bakeTexture(const QUrl & textureURL, image::TextureUsage::Type textureType, const QDir & outputDir, const QString & bakedFilename, const QByteArray & textureContent);
    QUrl _modelURL;
    QMultiHash<QUrl, QSharedPointer<TextureBaker>> _bakingTextures;
    TextureBakerThreadGetter _textureThreadGetter;
    void handleBakedTexture();
    QString texturePathRelativeToModel(QUrl fbxURL, QUrl textureURL);
    void checkIfTexturesFinished();
    void handleAbortedTexture();
    QString _originalOutputDir;
    bool _pendingErrorEmission{ false };
};

#endif