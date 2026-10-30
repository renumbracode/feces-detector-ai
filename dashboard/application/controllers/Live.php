<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class Live extends MY_Controller
{
    public function index()
    {
        /* The pen's calibration. The camera sits at one end of the pen and
         * the sprayer at the other, so the floor is seen in perspective; the
         * mapping from the picture to the pen's centimetres is a homography
         * fitted to the pen's four corners. A null matrix means the pen has
         * never been calibrated, and the live view falls back to its plain
         * percentage grid. */
        $this->load->library('Pen_calibration');
        $pen = $this->pen_calibration;

        $this->load->view('partials/header', ['pageTitle' => 'Live View', 'pageSlug' => 'live', 'streamUrl' => $this->stream_url()]);

        $data = [
            'streamUrl' => $this->stream_url(),
            'statusUrl' => $this->status_url(),
            'penMatrix' => $pen->matrix(),
            'penWidthCm' => $pen->width_cm(),
            'penLengthCm' => $pen->length_cm(),
            'penGridStepCm' => (float) $this->Setting_model->get('grid_step_cm', '5'),
            /* Whether the matrix came from a fit to the picture or from the
             * linear model, and which way the picture is turned: the two
             * behave differently if the camera is moved, so the page says
             * which is in use. */
            'penFitted' => $pen->is_fitted(),
            'penFlipY' => $pen->flip_y(),
            /* Where the calibration can be sent, so the operator does not
             * have to work it out. */
            'actuatorConfigUrl' => $this->actuator_config_url(),
        ];
        $this->load->view('live/index', $data);
        $this->load->view('partials/footer');
    }
}
