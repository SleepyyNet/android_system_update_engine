// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/omaha_response_handler_action.h"

#include <string>

#include <base/logging.h>
#include <base/strings/string_util.h>

#include "update_engine/constants.h"
#include "update_engine/delta_performer.h"
#include "update_engine/hardware_interface.h"
#include "update_engine/payload_state_interface.h"
#include "update_engine/prefs_interface.h"
#include "update_engine/utils.h"

using std::string;

namespace chromeos_update_engine {

const char OmahaResponseHandlerAction::kDeadlineFile[] =
    "/tmp/update-check-response-deadline";

OmahaResponseHandlerAction::OmahaResponseHandlerAction(
    SystemState* system_state)
    : system_state_(system_state),
      got_no_update_response_(false),
      key_path_(DeltaPerformer::kUpdatePayloadPublicKeyPath),
      deadline_file_(kDeadlineFile) {}

OmahaResponseHandlerAction::OmahaResponseHandlerAction(
    SystemState* system_state, const string& deadline_file)
    : system_state_(system_state),
      got_no_update_response_(false),
      key_path_(DeltaPerformer::kUpdatePayloadPublicKeyPath),
      deadline_file_(deadline_file) {}

void OmahaResponseHandlerAction::PerformAction() {
  CHECK(HasInputObject());
  ScopedActionCompleter completer(processor_, this);
  const OmahaResponse& response = GetInputObject();
  if (!response.update_exists) {
    got_no_update_response_ = true;
    LOG(INFO) << "There are no updates. Aborting.";
    return;
  }

  // Note: policy decision to not update to a version we rolled back from.
  string rollback_version =
      system_state_->payload_state()->GetRollbackVersion();
  if(!rollback_version.empty()) {
    LOG(INFO) << "Detected previous rollback from version " << rollback_version;
    if(rollback_version == response.version) {
      LOG(INFO) << "Received version that we rolled back from. Aborting.";
      return;
    }
  }

  // All decisions as to which URL should be used have already been done. So,
  // make the current URL as the download URL.
  string current_url = system_state_->payload_state()->GetCurrentUrl();
  if (current_url.empty()) {
    // This shouldn't happen as we should always supply the HTTPS backup URL.
    // Handling this anyway, just in case.
    LOG(ERROR) << "There are no suitable URLs in the response to use.";
    completer.set_code(kErrorCodeOmahaResponseInvalid);
    return;
  }

  install_plan_.download_url = current_url;
  install_plan_.version = response.version;

  OmahaRequestParams* params = system_state_->request_params();

  // If we're using p2p to download and there is a local peer, use it.
  if (params->use_p2p_for_downloading() && !params->p2p_url().empty()) {
    LOG(INFO) << "Replacing URL " << install_plan_.download_url
              << " with local URL " << params->p2p_url()
              << " since p2p is enabled.";
    install_plan_.download_url = params->p2p_url();
    system_state_->payload_state()->SetUsingP2PForDownloading(true);
  }

  // Fill up the other properties based on the response.
  install_plan_.payload_size = response.size;
  install_plan_.payload_hash = response.hash;
  install_plan_.metadata_size = response.metadata_size;
  install_plan_.metadata_signature = response.metadata_signature;
  install_plan_.public_key_rsa = response.public_key_rsa;
  install_plan_.hash_checks_mandatory = AreHashChecksMandatory(response);
  install_plan_.is_resume =
      DeltaPerformer::CanResumeUpdate(system_state_->prefs(), response.hash);
  if (install_plan_.is_resume) {
    system_state_->payload_state()->UpdateResumed();
  } else {
    system_state_->payload_state()->UpdateRestarted();
    LOG_IF(WARNING, !DeltaPerformer::ResetUpdateProgress(
        system_state_->prefs(), false))
        << "Unable to reset the update progress.";
    LOG_IF(WARNING, !system_state_->prefs()->SetString(
        kPrefsUpdateCheckResponseHash, response.hash))
        << "Unable to save the update check response hash.";
  }
  install_plan_.is_full_update = !response.is_delta_payload;

  TEST_AND_RETURN(utils::GetInstallDev(
      (!boot_device_.empty() ? boot_device_ :
          system_state_->hardware()->BootDevice()),
      &install_plan_.install_path));
  install_plan_.kernel_install_path =
      utils::KernelDeviceOfBootDevice(install_plan_.install_path);

  if (params->to_more_stable_channel() && params->is_powerwash_allowed())
    install_plan_.powerwash_required = true;


  TEST_AND_RETURN(HasOutputPipe());
  if (HasOutputPipe())
    SetOutputObject(install_plan_);
  LOG(INFO) << "Using this install plan:";
  install_plan_.Dump();

  // Send the deadline data (if any) to Chrome through a file. This is a pretty
  // hacky solution but should be OK for now.
  //
  // TODO(petkov): Rearchitect this to avoid communication through a
  // file. Ideally, we would include this information in D-Bus's GetStatus
  // method and UpdateStatus signal. A potential issue is that update_engine may
  // be unresponsive during an update download.
  utils::WriteFile(deadline_file_.c_str(),
                   response.deadline.data(),
                   response.deadline.size());
  chmod(deadline_file_.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  completer.set_code(kErrorCodeSuccess);
}

bool OmahaResponseHandlerAction::AreHashChecksMandatory(
    const OmahaResponse& response) {
  // All our internal testing uses dev server which doesn't generate
  // metadata signatures by default. So, in order not to break
  // image_to_live or other AU tools, we should waive the hash checks
  // for those cases, except if the response indicates that the
  // payload is signed.
  //
  // Since all internal testing is done using a dev_image or
  // test_image, we can use that as a criteria for waiving. This
  // criteria reduces the attack surface as opposed to waiving the
  // checks when we're in dev mode, because we do want to enforce the
  // hash checks when our end customers run in dev mode if they are
  // using an official build, so that they are protected more.
  if (!system_state_->hardware()->IsOfficialBuild()) {
    if (!response.public_key_rsa.empty()) {
      // The autoupdate_CatchBadSignatures test checks for this string
      // in log-files. Keep in sync.
      LOG(INFO) << "Mandating payload hash checks since Omaha Response "
                << "for unofficial build includes public RSA key.";
      return true;
    } else {
      LOG(INFO) << "Waiving payload hash checks for unofficial builds";
      return false;
    }
  }

  // If we're using p2p, |install_plan_.download_url| may contain a
  // HTTP URL even if |response.payload_urls| contain only HTTPS URLs.
  if (!StartsWithASCII(install_plan_.download_url, "https://", false)) {
    LOG(INFO) << "Mandating hash checks since download_url is not HTTPS.";
    return true;
  }

  // TODO(jaysri): VALIDATION: For official builds, we currently waive hash
  // checks for HTTPS until we have rolled out at least once and are confident
  // nothing breaks. chromium-os:37082 tracks turning this on for HTTPS
  // eventually.

  // Even if there's a single non-HTTPS URL, make the hash checks as
  // mandatory because we could be downloading the payload from any URL later
  // on. It's really hard to do book-keeping based on each byte being
  // downloaded to see whether we only used HTTPS throughout.
  for (size_t i = 0; i < response.payload_urls.size(); i++) {
    if (!StartsWithASCII(response.payload_urls[i], "https://", false)) {
      LOG(INFO) << "Mandating payload hash checks since Omaha response "
                << "contains non-HTTPS URL(s)";
      return true;
    }
  }

  LOG(INFO) << "Waiving payload hash checks since Omaha response "
            << "only has HTTPS URL(s)";
  return false;
}

}  // namespace chromeos_update_engine
