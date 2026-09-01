<?php

define('DB_HOST', 'localhost');
define('DB_NAME', 'feces_detector');
define('DB_USER', 'root');
define('DB_PASS', '');

define('BASE_URL', '/feces-detector-ai/dashboard');

function db(): PDO
{
    static $pdo = null;
    if ($pdo === null) {
        $pdo = new PDO(
            'mysql:host=' . DB_HOST . ';dbname=' . DB_NAME . ';charset=utf8mb4',
            DB_USER,
            DB_PASS,
            [
                PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            ]
        );
    }
    return $pdo;
}

function get_setting(string $key, string $default = ''): string
{
    static $cache = null;
    if ($cache === null) {
        $cache = [];
        foreach (db()->query('SELECT `key`, `value` FROM settings') as $row) {
            $cache[$row['key']] = $row['value'];
        }
    }
    return $cache[$key] ?? $default;
}

function set_setting(string $key, string $value): void
{
    $st = db()->prepare('INSERT INTO settings (`key`, `value`) VALUES (?, ?)
                         ON DUPLICATE KEY UPDATE `value` = VALUES(`value`)');
    $st->execute([$key, $value]);
}

function detector_url(string $path): string
{
    return 'http://' . get_setting('esp32_ip', '192.168.1.100') . $path;
}

function esp_url(string $path): string
{
    return detector_url($path);
}

function stream_url(): string
{
    $url = get_setting('stream_url', '');
    if ($url !== '') {
        return $url;
    }
    return esp_url('/stream');
}

function stream_status_url(): string
{
    $url = stream_url();
    $parsed = parse_url($url);
    $path = $parsed['path'] ?? '/';
    if (substr($path, -7) === '/stream') {
        $path = substr($path, 0, -7) . '/status';
    } else {
        $path = '/status';
    }
    $scheme = $parsed['scheme'] ?? 'http';
    $host = $parsed['host'] ?? '';
    $port = isset($parsed['port']) ? ':' . $parsed['port'] : '';
    return $scheme . '://' . $host . $port . $path;
}

function http_get(string $url, int $timeout = 3): ?string
{
    $ctx = stream_context_create(['http' => ['timeout' => $timeout, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    return $body === false ? null : $body;
}
