<?php

/**
 * @file
 * Drush commands for the islandora_job module.
 */

/**
 * Implements hook_drush_command().
 */
function islandora_job_drush_command() {
  $items = array();
  $items['islandora-job-router'] = array(
    'description' => t('Job routing function for Islandora that is executed by the worker processes.  There is no need to ever execute this yourself.'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_LOGIN,
  );
  $items['islandora-job-list-functions'] = array(
    'description' => t('Lists all job functions.'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_LOGIN,
    'options' => array(
      'param_format' => array(
        'description' => t('Flag to format as gearman server params, prefixed by "-f". Functions are otherwise newline-separated.'),
        'required' => FALSE,
      ),
    ),
  );
  return $items;
}

/**
 * Routes a payload received from Gearman to the appropriate job function.
 *
 * Reads STDIN for JSON containing the function to execute, the module it comes
 * from, and the arguments to supply it.
 */
function drush_islandora_job_router() {
  // XXX: Due to how Drush bootstrapping works, the connection may be created
  // without credentials (when your site's front page is
  // 'islandora/object/some:object', for example). Resetting to ensure a new
  // connection gets created should fix it.
  module_load_include('inc', 'islandora_job', 'includes/db');
  module_load_include('inc', 'islandora_job', 'includes/utilities');
  drupal_static_reset('islandora_get_tuque_connection');
  // Parse the payload into an associative array.
  $raw_payload = stream_get_contents(STDIN);
  $payload = json_decode($raw_payload, TRUE);
  // Get the list of all available jobs.
  $job_registry = module_invoke_all("islandora_job_register_jobs");

  $func_name = islandora_job_get_full_func_name($payload['func']);
  $args = $payload['args'];

  // Dynamically execute the job function, if it's in the list.
  // Echo out the results so there's a return value for foreground jobs.
  if (isset($job_registry[$func_name])) {
    $job = $job_registry[$func_name];
    $type = $job['type'];
    $module = $job['module'];
    $name = isset($job['name']) ? $job['name'] : NULL;
    $status_id = islandora_job_start_job_status($raw_payload);
    register_shutdown_function('islandora_job_router_job_error', $status_id, $func_name, $args);
    module_load_include($type, $module, $name);
    try {
      $return = call_user_func_array($func_name, $args);
      islandora_job_remove_job_status($status_id);
      echo $return;
    }
    catch (Exception $e) {
      drush_set_error('islandora_job', dt('The callback !func_name with args !args threw an exception.', array(
        '!func_name' => $func_name,
        '!args' => implode(', ', $args),
      )));
      islandora_job_update_job_status($status_id, ISLANDORA_JOB_STATE__ERROR);
      throw $e;
    }
  }
}

/**
 * Prints a list of multisite-aware job names out to the command line.
 */
function drush_islandora_job_list_functions() {
  module_load_include('inc', 'islandora_job', 'includes/utilities');
  $format = drush_get_option('param_format', FALSE);

  $functions = array_map('islandora_job_function_name', islandora_job_get_aliased_function_names());
  $imploder = $format ? ' ' : "\n";
  if ($format) {
    $add_prefix = function (&$function) {
      $function = "-f $function";
    };
    array_walk($functions, $add_prefix);
  }
  drush_print(implode($imploder, $functions));
}

/**
 * Shutdown callback to update job status in the event of an error.
 *
 * @param int $status_id
 *   The status ID of the job to be updated.
 * @param string $name
 *   The name of the job.
 * @param array $args
 *   The arguments that were passed to the job.
 */
function islandora_job_router_job_error($status_id, $name, array $args) {
  $error = error_get_last();

  $applicable_errors = array(
    E_ERROR,
    E_PARSE,
    E_RECOVERABLE_ERROR,
  );
  if ($error != NULL && in_array($error['type'], $applicable_errors)) {
    drush_set_error('islandora_job', dt('The callback !func_name with args !args threw an exception: !exception in !file on line !line.', array(
      '!func_name' => $name,
      '!args' => implode(', ', $args),
      '!exception' => isset($error['message']) ? $error['message'] : '',
      '!file' => isset($error['file']) ? $error['file'] : '',
      '!line' => isset($error['line']) ? $error['line'] : '',
    )));
    islandora_job_update_job_status($status_id, ISLANDORA_JOB_STATE__ERROR);
  }
}
