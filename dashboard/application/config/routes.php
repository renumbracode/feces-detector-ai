<?php
defined('BASEPATH') OR exit('No direct script access allowed');

$route['default_controller'] = 'dashboard';
$route['404_override'] = '';
$route['translate_uri_dashes'] = FALSE;

$route['live'] = 'live';
$route['history'] = 'history';
$route['control'] = 'control';
$route['api/latest'] = 'api/latest';
$route['api/insert'] = 'api/insert';
$route['api/verify'] = 'api/verify';
$route['api/status'] = 'api/status';
$route['api/calibrate'] = 'api/calibrate';
$route['api/push_calibration'] = 'api/push_calibration';
$route['api/cameras'] = 'api/cameras';
