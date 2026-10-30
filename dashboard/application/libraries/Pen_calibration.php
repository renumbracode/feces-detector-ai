<?php
defined('BASEPATH') OR exit('No direct script access allowed');

/**
 * The pen is the calibration target.
 *
 * The camera sits at one end of the pigpen and the sprayer at the other, so
 * the floor is seen in PERSPECTIVE: a pixel's size in centimetres depends on
 * how far down the pen it is. A flat "pixels to centimetres" factor would be
 * out by up to a factor of two across the pen, which is why the mapping here
 * is a homography instead -- it is fitted to the four corners of a pen whose
 * real size is known (20 cm x 30 cm), so it needs no camera height, no tilt
 * angle and no lens field of view to be guessed.
 *
 * The matrix is stored as eight numbers: it maps pen centimetres to a
 * fraction of the image, with the bottom-right term fixed at 1. The inverse
 * (image fraction -> pen centimetres) is derived from it, so the server never
 * solves anything -- it only applies a matrix the browser already fitted.
 *
 * Coordinates are TOP-LEFT, the same convention the wire format and the
 * detections table use.
 */
class Pen_calibration
{
    /** @var float[]|null The forward matrix, 8 numbers: h0..h7. */
    private $h = null;
    /** @var float[]|null The inverse matrix, 8 numbers. */
    private $inv = null;
    /** True when the matrix came from a fit rather than the linear model. */
    private $fitted = false;
    private $widthCm = 0.0;
    private $lengthCm = 0.0;
    /** True when the camera is at the TOP of the picture. */
    private $flipY = false;
    /** Image width/height the fit was made against; 0 when unknown. */
    private $aspect = 0.0;

    public function __construct()
    {
        $this->load();
    }

    /** Re-read the settings. Call after changing the pen size. */
    public function load()
    {
        $this->h = null;
        $this->inv = null;
        $this->fitted = false;
        $this->widthCm = (float) $this->setting('pen_width_cm', '20');
        $this->lengthCm = (float) $this->setting('pen_length_cm', '30');
        $this->flipY = ((string) $this->setting('pen_flip_y', '0')) === '1';
        $this->aspect = (float) $this->setting('pen_calib_aspect', '0');

        $raw = trim((string) $this->setting('pen_homography', ''));
        $h = self::parse_matrix($raw);
        if ($h === null) {
            /* No fit stored: the linear model stands, as long as the pen's
             * size is known. Without a size there is nothing to convert to,
             * and the caller falls back to the percentage grid rather than
             * inventing a scale. */
            $def = self::default_matrix($this->widthCm, $this->lengthCm, $this->flipY);
            if ($def === null) {
                return;
            }
            $this->h = $def;
            $this->inv = self::invert($def);
            return;
        }
        $inv = self::invert($h);
        if ($inv === null) {
            return;
        }
        $this->h = $h;
        $this->inv = $inv;
        $this->fitted = true;
    }

    /**
     * The linear model: the picture IS the pen.
     *
     * The camera looks straight down at the pen, so the pen appears in the
     * picture as a rectangle and every centimetre is the same size as every
     * other one. The picture's left edge is the pen's left, and the picture's
     * BOTTOM is the pen's near end -- the end the camera is at.
     *
     * As a homography (pen cm -> image fraction) that is:
     *     u = X / width
     *     v = 1 - Y / length
     * which is affine, so the same code that fits a perspective handles it
     * with no special case anywhere downstream.
     *
     * Returns null when the pen's size is not known.
     */
    public static function default_matrix($widthCm, $lengthCm, $flipY = false)
    {
        $widthCm = (float) $widthCm;
        $lengthCm = (float) $lengthCm;
        if ($widthCm <= 0 || $lengthCm <= 0) {
            return null;
        }
        if ($flipY) {
            /* The camera is at the TOP of the picture: the pen's near end is
             * at the top, so v counts from there. */
            return [
                1.0 / $widthCm, 0.0, 0.0,
                0.0, 1.0 / $lengthCm, 0.0,
                0.0, 0.0,
            ];
        }
        return [
            1.0 / $widthCm, 0.0, 0.0,
            0.0, -1.0 / $lengthCm, 1.0,
            0.0, 0.0,
        ];
    }

    private function setting($key, $default)
    {
        $ci =& get_instance();
        return $ci->Setting_model->get($key, $default);
    }

    /* ----- state ----- */

    public function is_calibrated()
    {
        return $this->h !== null;
    }

    /** True when the matrix came from a fit to the picture, rather than
     * from the linear model. The live view says which is in use, because
     * they behave differently if the camera is moved. */
    public function is_fitted()
    {
        return $this->fitted;
    }

    /** True when the camera is at the top of the picture. */
    public function flip_y()
    {
        return $this->flipY;
    }

    public function matrix()
    {
        return $this->h;
    }

    public function width_cm()
    {
        return $this->widthCm;
    }

    public function length_cm()
    {
        return $this->lengthCm;
    }

    public function aspect()
    {
        return $this->aspect;
    }

    /* ----- the transform ----- */

    /** Pen centimetres -> image fraction (u, v), both 0..1. */
    public function cm_to_image($cmX, $cmY)
    {
        if ($this->h === null) {
            return null;
        }
        $h = $this->h;
        $w = $h[6] * $cmX + $h[7] * $cmY + 1.0;
        if (abs($w) < 1e-9) {
            return null;
        }
        return [
            ($h[0] * $cmX + $h[1] * $cmY + $h[2]) / $w,
            ($h[3] * $cmX + $h[4] * $cmY + $h[5]) / $w,
        ];
    }

    /** Image fraction (u, v) -> pen centimetres (X, Y). */
    public function image_to_cm($u, $v)
    {
        if ($this->inv === null) {
            return null;
        }
        $g = $this->inv;
        $w = $g[6] * $u + $g[7] * $v + 1.0;
        if (abs($w) < 1e-9) {
            return null;
        }
        return [
            ($g[0] * $u + $g[1] * $v + $g[2]) / $w,
            ($g[3] * $u + $g[4] * $v + $g[5]) / $w,
        ];
    }

    /**
     * A normalized box (top-left, 0..1) -> centimetres on the pen.
     *
     * x_cm/y_cm place the box's TOP-LEFT corner, the same corner the
     * normalized columns describe. w_cm/h_cm are the box's extent along the
     * pen's two axes: in perspective the projected quad is not a rectangle,
     * so this is the axis-aligned envelope of the four projected corners.
     *
     * Returns null when there is no calibration, so a caller can leave the
     * columns NULL rather than invent a position.
     */
    public function box_to_cm($x, $y, $w, $h)
    {
        if ($this->inv === null) {
            return null;
        }
        $x = (float) $x;
        $y = (float) $y;
        $w = (float) $w;
        $h = (float) $h;

        $corners = [];
        foreach ([[$x, $y], [$x + $w, $y], [$x + $w, $y + $h], [$x, $y + $h]] as $c) {
            $p = $this->image_to_cm($c[0], $c[1]);
            if ($p === null) {
                return null;
            }
            $corners[] = $p;
        }

        $minX = $minY = INF;
        $maxX = $maxY = -INF;
        foreach ($corners as $p) {
            $minX = min($minX, $p[0]);
            $maxX = max($maxX, $p[0]);
            $minY = min($minY, $p[1]);
            $maxY = max($maxY, $p[1]);
        }

        $tl = $this->image_to_cm($x, $y);
        return [
            'x' => $tl[0],
            'y' => $tl[1],
            'w' => $maxX - $minX,
            'h' => $maxY - $minY,
        ];
    }

    /**
     * Does the picture the device published match the one the fit was made
     * against? A different aspect means the normalized coordinates describe a
     * differently shaped image, so every centimetre derived from them would
     * be wrong. Returns true when there is nothing to compare against.
     *
     * The linear model does not care: its proportions come from the pen's
     * size, not from the shape of the picture.
     */
    public function aspect_matches($frameW, $frameH)
    {
        if (!$this->fitted || $this->aspect <= 0 || !$frameW || !$frameH) {
            return true;
        }
        $incoming = (float) $frameW / (float) $frameH;
        return abs($incoming - $this->aspect) <= 0.02 * $this->aspect;
    }

    /* ----- matrix plumbing ----- */

    /** Parse eight comma-separated numbers. Returns null when malformed. */
    public static function parse_matrix($raw)
    {
        $raw = trim((string) $raw);
        if ($raw === '') {
            return null;
        }
        $parts = explode(',', $raw);
        if (count($parts) !== 8) {
            return null;
        }
        $out = [];
        foreach ($parts as $p) {
            $p = trim($p);
            if ($p === '' || !is_numeric($p)) {
                return null;
            }
            $out[] = (float) $p;
        }
        return $out;
    }

    /** Format a matrix for storage. */
    public static function format_matrix($h)
    {
        $parts = [];
        foreach ($h as $v) {
            $parts[] = rtrim(rtrim(sprintf('%.10F', (float) $v), '0'), '.');
        }
        return implode(',', $parts);
    }

    /**
     * The inverse of the 3x3 homography, as 8 numbers with the bottom-right
     * term fixed at 1.
     *
     * The matrix is
     *     [ h0 h1 h2 ]
     *     [ h3 h4 h5 ]
     *     [ h6 h7  1 ]
     * and the inverse comes from the adjugate divided by the determinant,
     * then renormalised so its bottom-right term is 1 again.
     */
    public static function invert($h)
    {
        if (!is_array($h) || count($h) !== 8) {
            return null;
        }
        $v = array_map('floatval', $h);

        /* The matrix, written out:
         *     [ m00 m01 m02 ]
         *     [ m10 m11 m12 ]
         *     [ m20 m21  1  ]     <- the bottom-right term is fixed at 1
         */
        $m00 = $v[0]; $m01 = $v[1]; $m02 = $v[2];
        $m10 = $v[3]; $m11 = $v[4]; $m12 = $v[5];
        $m20 = $v[6]; $m21 = $v[7]; $m22 = 1.0;

        /* Cofactors. */
        $c00 = $m11 * $m22 - $m12 * $m21;
        $c01 = -($m10 * $m22 - $m12 * $m20);
        $c02 = $m10 * $m21 - $m11 * $m20;
        $c10 = -($m01 * $m22 - $m02 * $m21);
        $c11 = $m00 * $m22 - $m02 * $m20;
        $c12 = -($m00 * $m21 - $m01 * $m20);
        $c20 = $m01 * $m12 - $m02 * $m11;
        $c21 = -($m00 * $m12 - $m02 * $m10);
        $c22 = $m00 * $m11 - $m01 * $m10;

        /* adjugate = transpose of the cofactor matrix. */
        $adj = [
            [$c00, $c10, $c20],
            [$c01, $c11, $c21],
            [$c02, $c12, $c22],
        ];

        $det = $m00 * $c00 + $m01 * $c01 + $m02 * $c02;
        if (abs($det) < 1e-12) {
            return null;
        }

        $inv = [];
        for ($r = 0; $r < 3; $r++) {
            for ($col = 0; $col < 3; $col++) {
                $inv[$r][$col] = $adj[$r][$col] / $det;
            }
        }

        /* Renormalise so the bottom-right term is 1, matching the convention
         * the forward matrix is stored in. */
        $k = $inv[2][2];
        if (abs($k) < 1e-12) {
            return null;
        }
        $out = [];
        for ($r = 0; $r < 3; $r++) {
            for ($col = 0; $col < 3; $col++) {
                $out[$r][$col] = $inv[$r][$col] / $k;
            }
        }

        return [
            $out[0][0], $out[0][1], $out[0][2],
            $out[1][0], $out[1][1], $out[1][2],
            $out[2][0], $out[2][1],
        ];
    }
}
