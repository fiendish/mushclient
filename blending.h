#pragma once

// blending.h
//  Defines for: CMiniWindow::BlendImage
//    and        CMUSHclientDoc::BlendPixel

// Match the original alpha draw rule for NaN, including in Release builds.
#pragma float_control (precise, on, push)
inline bool NormalizeBlendOpacity (double & opacity)
  {
  if (opacity < 0.0 || opacity > 1.0)
    return false;
  if (!(opacity >= 0.0))
    opacity = 1.0;
  return true;
  }
#pragma float_control (pop)

// colour stuff

#define CLAMP(x) (((x) < 0) ? 0 : (x > 255) ? 255 : (x))


// see: http://www.nathanm.com/photoshop-blending-math/
// and: http://www.pegtop.net/delphi/articles/blendmodes/                   

#define uint8 unsigned char
#define sqr(x) ((x)*(x))    // square of x

// A = blend, B = base

#define Blend_Normal(A,B)	((uint8)(A))
#define Blend_Lighten(A,B)	((uint8)((B > A) ? B:A))
#define Blend_Darken(A,B)	((uint8)((B > A) ? A:B))
#define Blend_Multiply(A,B)	((uint8)((A * B) / 255))
#define Blend_Average(A,B)	((uint8)((A + B) / 2))
#define Blend_Add(A,B)	((uint8)((A + B > 255) ? 255:(A + B)))
#define Blend_Subtract(A,B)	((uint8)((A + B < 255) ? 0:(A + B - 255)))
#define Blend_Difference(A,B)	((uint8)(abs(A - B)))
#define Blend_Negation(A,B)	((uint8)(255 - abs(255 - A - B)))
#define Blend_Screen(A,B)	((uint8)(255 - (((255 - A) * (255 - B)) >> 8)))
#define Blend_Exclusion(A,B)	((uint8)(A + B - 2 * A * B / 255))
#define Blend_Overlay(A,B)	((uint8)((B < 128) ? (2 * A * B / 255):(255 - 2 * (255 - A) * (255 - B) / 255)))
#define Blend_SoftLight(A,B)	((uint8) ((A * B) >> 8) +  ((B * (255 - (((255 - B) * (255-A)) >> 8) - ((A * B) >> 8) )) >> 8) )           // c = ((A * B) >> 8)
#define Blend_HardLight(A,B)	(Blend_Overlay(B,A))
#define Blend_ColorDodge(A,B)	((uint8)((A == 255) ? A:((((unsigned long) B) << 8 ) / (255 - A) > 255) ? 255:((((unsigned long) B) << 8 ) / (255 - A))))
#define Blend_ColorBurn(A,B)	((uint8)((A == 0) ? 0:((255 - (( ((unsigned long)(255 - B)) << 8 ) / A)) < 0) ? 0:(255 - (( ((unsigned long)(255 - B)) << 8 ) / A))))
#define Blend_LinearDodge(A,B)	(Blend_Add(A,B))
#define Blend_LinearBurn(A,B)	(Blend_Subtract(A,B))
#define Blend_LinearLight(A,B)	((uint8)(A < 128) ? Blend_LinearBurn((2 * A),B):Blend_LinearDodge((2 * (A - 128)),B))
#define Blend_VividLight(A,B)	((uint8)(A < 128) ? Blend_ColorBurn((2 * A),B):Blend_ColorDodge((2 * (A - 128)),B))
#define Blend_PinLight(A,B)	((uint8)(A < 128) ? Blend_Darken((2 * A),B):Blend_Lighten((2 *(A - 128)),B))
#define Blend_HardMix(A,B)	((uint8)(A < 255 - B) ? 0:255)
#define Blend_Reflect(A,B)	((uint8)((B == 255) ? B:((A * A / (255 - B) > 255) ? 255:(A * A / (255 - B)))))
#define Blend_Glow(A,B)	(Blend_Reflect(B,A))
#define Blend_Phoenix(A,B)	((uint8)(min(A,B) - max(A,B) + 255))
#define Blend_Opacity(A,B,F,O)	((uint8)(O * F(A,B) + (1 - O) * B + 0.5))

#define Simple_Opacity(B,X,O)  ((uint8)(O * X + (1 - O) * B + 0.5))

#define Blend_InverseColorDodge(A,B)	Blend_ColorDodge (B, A)
#define Blend_InverseColorBurn(A,B)	Blend_ColorBurn (B, A)
#define Blend_Freeze(A,B)	((uint8)((A == 0) ? A:((255 - sqr(255 - B)/ A < 0) ? 0:(255 - sqr(255 - B) / A))))
#define Blend_Heat(A,B)	 Blend_Freeze (B, A)
#define Blend_Stamp(A,B)	((uint8)((B + 2*A - 256 < 0) ? 0 : (B + 2*A - 256 > 255) ? 255 : (B + 2*A - 256)    ))
#define Blend_Interpolate(A,B)	((uint8)((cos_table [A] + cos_table [B]> 255) ? 255 : (cos_table [A] + cos_table [B])  ))

#define Blend_Xor(A,B)	((uint8) A ^ B )
#define Blend_And(A,B)	((uint8) A & B )
#define Blend_Or(A,B)	((uint8) A | B )
#define Blend_A(A,B)	((uint8) (A))
#define Blend_B(A,B)	((uint8) (B))


// Each entry gives the script mode number, Lua name and blend operation.
// Keep dissolve in its original position in each switch. Moving it changes
// how the compiler lays out the pixel loops.
// COLOUR entries give red, green and blue. A is the source and B is the base.
// Keep the conditionals in modes 42 to 50 unparenthesized. Simple_Opacity
// relies on their original operator precedence at partial opacity.
#define MUSHCLIENT_BLEND_MODES(BASIC, CHANNEL, COLOUR, SPECIAL) \
  BASIC (1, "blend_normal", Blend_Normal) \
  BASIC (2, "blend_average", Blend_Average) \
  BASIC (3, "blend_interpolate", Blend_Interpolate) \
  SPECIAL (4, "blend_dissolve", BlendDissolveMode) \
  CHANNEL (5, "blend_darken", Blend_Darken) \
  CHANNEL (6, "blend_multiply", Blend_Multiply) \
  CHANNEL (7, "blend_colour_burn", Blend_ColorBurn) \
  CHANNEL (8, "blend_linear_burn", Blend_LinearBurn) \
  CHANNEL (9, "blend_inverse_colour_burn", Blend_InverseColorBurn) \
  CHANNEL (10, "blend_subtract", Blend_Subtract) \
  CHANNEL (11, "blend_lighten", Blend_Lighten) \
  CHANNEL (12, "blend_screen", Blend_Screen) \
  CHANNEL (13, "blend_colour_dodge", Blend_ColorDodge) \
  CHANNEL (14, "blend_linear_dodge", Blend_LinearDodge) \
  CHANNEL (15, "blend_inverse_colour_dodge", Blend_InverseColorDodge) \
  CHANNEL (16, "blend_add", Blend_Add) \
  CHANNEL (17, "blend_overlay", Blend_Overlay) \
  CHANNEL (18, "blend_soft_light", Blend_SoftLight) \
  CHANNEL (19, "blend_hard_light", Blend_HardLight) \
  CHANNEL (20, "blend_vivid_light", Blend_VividLight) \
  CHANNEL (21, "blend_linear_light", Blend_LinearLight) \
  CHANNEL (22, "blend_pin_light", Blend_PinLight) \
  CHANNEL (23, "blend_hard_mix", Blend_HardMix) \
  CHANNEL (24, "blend_difference", Blend_Difference) \
  CHANNEL (25, "blend_exclusion", Blend_Exclusion) \
  CHANNEL (26, "blend_reflect", Blend_Reflect) \
  CHANNEL (27, "blend_glow", Blend_Glow) \
  CHANNEL (28, "blend_freeze", Blend_Freeze) \
  CHANNEL (29, "blend_heat", Blend_Heat) \
  CHANNEL (30, "blend_negation", Blend_Negation) \
  CHANNEL (31, "blend_phoenix", Blend_Phoenix) \
  CHANNEL (32, "blend_stamp", Blend_Stamp) \
  CHANNEL (33, "blend_xor", Blend_Xor) \
  CHANNEL (34, "blend_and", Blend_And) \
  CHANNEL (35, "blend_or", Blend_Or) \
  COLOUR (36, "blend_red", rA, gB, bB) \
  COLOUR (37, "blend_green", rB, gA, bB) \
  COLOUR (38, "blend_blue", rB, gB, bA) \
  COLOUR (39, "blend_yellow", rA, gA, bB) \
  COLOUR (40, "blend_cyan", rB, gA, bA) \
  COLOUR (41, "blend_magenta", rA, gB, bA) \
  COLOUR (42, "blend_green_limited_by_red", rA, (gA > rA) ? rA : gA, bA) \
  COLOUR (43, "blend_green_limited_by_blue", rA, (gA > bA) ? bA : gA, bA) \
  COLOUR (44, "blend_green_limited_by_average_of_red_and_blue", rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA) \
  COLOUR (45, "blend_blue_limited_by_red", rA, gA, (bA > rA) ? rA : bA) \
  COLOUR (46, "blend_blue_limited_by_green", rA, gA, (bA > gA) ? gA : bA) \
  COLOUR (47, "blend_blue_limited_by_average_of_red_and_green", rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA) \
  COLOUR (48, "blend_red_limited_by_green", (rA > gA) ? gA : rA, gA, bA) \
  COLOUR (49, "blend_red_limited_by_blue", (rA > bA) ? bA : rA, gA, bA) \
  COLOUR (50, "blend_red_limited_by_average_of_green_and_blue", (rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA) \
  COLOUR (51, "blend_red_only", rA, 0, 0) \
  COLOUR (52, "blend_green_only", 0, gA, 0) \
  COLOUR (53, "blend_blue_only", 0, 0, bA) \
  COLOUR (54, "blend_discard_red", 0, gA, bA) \
  COLOUR (55, "blend_discard_green", rA, 0, bA) \
  COLOUR (56, "blend_discard_blue", rA, gA, 0) \
  COLOUR (57, "blend_all_red", rA, rA, rA) \
  COLOUR (58, "blend_all_green", gA, gA, gA) \
  COLOUR (59, "blend_all_blue", bA, bA, bA) \
  SPECIAL (60, "blend_hue_mode", BlendHueMode) \
  SPECIAL (61, "blend_saturation_mode", BlendSaturationMode) \
  SPECIAL (62, "blend_colour_mode", BlendColourMode) \
  SPECIAL (63, "blend_luminance_mode", BlendLuminanceMode) \
  SPECIAL (64, "blend_hsl", BlendHslMode)

#define SKIP_BLEND_MODE(...)
#define NAME_BLEND_MODE(number, script_name, mode) mode = number,
#define COUNT_BLEND_MODE(...) + 1
enum
  {
  MUSHCLIENT_BLEND_MODES (SKIP_BLEND_MODE, SKIP_BLEND_MODE, SKIP_BLEND_MODE, NAME_BLEND_MODE)
  BlendModeCount = 0 MUSHCLIENT_BLEND_MODES (COUNT_BLEND_MODE, COUNT_BLEND_MODE,
                                            COUNT_BLEND_MODE, COUNT_BLEND_MODE)
  };
#undef COUNT_BLEND_MODE
#undef NAME_BLEND_MODE
