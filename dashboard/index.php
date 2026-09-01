<?php
/**
 * CodeIgniter 3 front controller for the Pigpen Feces Detector dashboard.
 *
 * This file expects a CodeIgniter 3 `system/` folder next to it (the official
 * framework core). Download it from https://codeigniter.com/download and
 * extract the `system/` directory here.
 *
 *       dashboard/
 *       ├── index.php          <-- you are here
 *       ├── system/            <-- drop the CI3 framework core here
 *       └── application/       <-- our MVC app (this repo)
 */

define('ENVIRONMENT', isset($_SERVER['CI_ENV']) ? $_SERVER['CI_ENV'] : 'development');

switch (ENVIRONMENT) {
    case 'development':
        error_reporting(-1);
        ini_set('display_errors', 1);
        break;
    case 'testing':
    case 'production':
        ini_set('display_errors', 0);
        if (version_compare(PHP_VERSION, '5.3', '>=')) {
            error_reporting(E_ALL & ~E_NOTICE & ~E_DEPRECATED & ~E_STRICT & ~E_USER_NOTICE & ~E_USER_DEPRECATED);
        } else {
            error_reporting(E_ALL & ~E_NOTICE & ~E_STRICT & ~E_USER_NOTICE);
        }
        break;
    default:
        header('HTTP/1.1 503 Service Unavailable.', true, 503);
        echo 'The application environment is not set correctly.';
        exit(1);
}

$system_path = 'system';
$application_folder = 'application';

if (defined('STDIN')) {
    chdir(dirname(__FILE__));
}

if (($_temp = realpath($system_path)) !== false) {
    $system_path = $_temp.'/';
} else {
    $system_path = rtrim($system_path, '/').'/';
}

if (!is_dir($system_path)) {
    header('HTTP/1.1 503 Service Unavailable.', true, 503);
    echo 'Your system folder path does not appear to be set correctly. '
       . 'Place the CodeIgniter 3 system/ directory here first.';
    exit(3);
}

define('SELF', pathinfo(__FILE__, PATHINFO_BASENAME));
define('BASEPATH', $system_path);
define('FCPATH', dirname(__FILE__).DIRECTORY_SEPARATOR);
define('SYSDIR', basename($system_path));
define('APPPATH', $application_folder.DIRECTORY_SEPARATOR);
define('VIEWPATH', $application_folder.'/views'.DIRECTORY_SEPARATOR);

require_once BASEPATH.'core/CodeIgniter.php';
