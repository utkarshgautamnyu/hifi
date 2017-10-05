#ifndef hifi_ModelBaker_h
#define hifi_ModelBaker_h

#include <FBX.h>
#include "Baker.h"

class ModelBaker : public Baker {
    Q_OBJECT
public:
    ModelBaker();
    static FBXNode compressMesh(FBXMesh& mesh);
};

#endif