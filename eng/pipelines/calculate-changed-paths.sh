#!/usr/bin/env bash

# Disable globbing in this bash script since we iterate over path patterns
set -f

source="${BASH_SOURCE[0]}"

# resolve $source until the file is no longer a symlink
while [[ -h "$source" ]]; do
  scriptroot="$( cd -P "$( dirname "$source" )" && pwd )"
  source="$(readlink "$source")"
  # if $source was a relative symlink, we need to resolve it relative to the path where the
  # symlink file was located
  [[ $source != /* ]] && source="$scriptroot/$source"
done

scriptroot="$( cd -P "$( dirname "$source" )" && pwd )"
eng_root=`cd -P "$scriptroot/.." && pwd`

libraries_exclude_paths=()
coreclr_exclude_paths=()
installer_exclude_paths=()
libraries_include_paths=()
coreclr_include_paths=()
installer_include_paths=()
ci=true
diff_target=""

while [[ $# > 0 ]]; do
  opt="$(echo "${1/#--/-}" | awk '{print tolower($0)}')"
  case "$opt" in
    -help|-h)
      usage
      exit 0
      ;;
    -difftarget)
      diff_target=$2
      shift
      ;;
    -librariesexclude)
      IFS='+' read -r -a tmp <<< $2
      libraries_exclude_paths+=($tmp)
      shift
      ;;
    -librariesinclude)
      IFS='+' read -r -a tmp <<< $2
      libraries_include_paths+=($tmp)
      shift
      ;;
    -coreclrexclude)
      IFS='+' read -r -a tmp <<< $2
      coreclr_exclude_paths+=($tmp)
      shift
      ;;
    -coreclrinclude)
      IFS='+' read -r -a tmp <<< $2
      coreclr_include_paths+=($tmp)
      shift
      ;;
    -installerexclude)
      IFS='+' read -r -a tmp <<< $2
      installer_exclude_paths+=($tmp)
      shift
      ;;
    -installerinclude)
      IFS='+' read -r -a tmp <<< $2
      installer_include_paths+=($tmp)
      shift
      ;;
  esac

  shift
done

. "$eng_root/common/pipeline-logging-functions.sh"

# expected args
# $@: filter string
function runGitDiff {
  local _filter=$@
  echo ""
  echo "git diff -M -C -b --ignore-cr-at-eol --ignore-space-at-eol --exit-code --quiet $diff_target -- $_filter"
  git diff -M -C -b --ignore-cr-at-eol --ignore-space-at-eol --exit-code --quiet $diff_target -- $_filter
  git_diff_exit_code=$?
}

# expected args
# $1: subset
# $@: filter string
function printMatchedPaths {
  local _subset=$1
  shift
  local _filter=$@
  echo ""
  echo "----- Matching files for $_subset -----"
  git diff -M -C -b --ignore-cr-at-eol --ignore-space-at-eol --name-only $diff_target -- $_filter
}

# expected args
# $1: subset name
# $2: azure devops variable name
function probePaths {
  local _subset=$1
  local exclude_array_name="${_subset}_exclude_paths[@]"
  local exclude_paths=("${!exclude_array_name}")
  local include_array_name="${_subset}_include_paths[@]"
  local include_paths=("${!include_array_name}")
  local _azure_devops_var_name=$2
  local exclude_path_string=""
  local include_path_string=""
  local found_applying_changes=false
  
  echo ""
  echo "******* Probing $_subset exclude paths *******";
  for _path in "${exclude_paths[@]}"; do
    echo "$_path"
    if [[ "$exclude_path_string" == "" ]]; then
      exclude_path_string=":!$_path"
    else
      exclude_path_string="$exclude_path_string :!$_path"
    fi
  done

  runGitDiff $exclude_path_string
  if [[ "$git_diff_exit_code" == "1" ]]; then
    found_applying_changes=true
    printMatchedPaths $_subset $exclude_path_string
  fi

  if [[ $found_applying_changes != true && ${#include_paths[@]} -gt 0 ]]; then
    echo ""
    echo "******* Probing $_subset include paths *******";
    for _path in "${include_paths[@]}"; do
      echo "$_path"
      if [[ "$include_path_string" == "" ]]; then
        include_path_string=":$_path"
      else
        include_path_string="$exclude_path_string :$_path"
      fi
    done

    runGitDiff $include_path_string
    if [[ "$git_diff_exit_code" == "1" ]]; then
      found_applying_changes=true
      printMatchedPaths $_subset $include_path_string
    fi
  fi

  if [[ $found_applying_changes == true ]]; then
    echo ""
    echo "Setting pipeline variable $_azure_devops_var_name=true"
    Write-PipelineSetVariable -name $_azure_devops_var_name -value true
  else
    echo ""
    echo "No changed files for $_subset"
  fi
}

probePaths "libraries" "containsLibrariesChange"
probePaths "coreclr" "containsCoreclrChange"
probePaths "installer" "containsInstallerChange"
