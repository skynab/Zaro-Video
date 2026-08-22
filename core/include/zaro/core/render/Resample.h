#pragma once

#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Scale a picture into a differently-sized one by averaging over source
/// boxes.
///
/// **Area averaging, not bilinear.** A bilinear sample reads four pixels
/// however far apart they are, so halving a frame twice throws away three
/// quarters of the picture and keeps whichever pixels the grid happened to land
/// on -- which is what makes a shrunk still of a brick wall shimmer. Averaging
/// the whole box each output pixel covers is the cheapest thing that keeps the
/// detail as detail.
///
/// **On premultiplied values**, like the blur and for the same reason: an
/// average of straight colours pulls the colour of transparent pixels into the
/// visible ones.
///
/// Enlarging is not what this is for -- box averaging an enlargement gives
/// nearest-neighbour blocks -- and it says so by falling back to bilinear when
/// the destination is bigger.
void resizeInto(const RgbaImage& from, RgbaImage& to);

}  // namespace zaro::render
