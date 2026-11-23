#include "puck/puck_3d.h"

namespace pk {

bool Puck3D::initialize(std::string* error) {
    if (error) {
        *error = "the GLES 3.0 Puck renderer is planned for M1";
    }
    return false;
}

}  // namespace pk
