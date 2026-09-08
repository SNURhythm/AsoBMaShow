module UI
  def self.user_error!(message)
    raise message
  end
end

module SharedValues
  IPA_OUTPUT_PATH = :ipa
end

def default_platform(*)
end

def platform(*)
  yield
end

def desc(*)
end

def lane(name, &block)
  (@lanes ||= {})[name] = block
end

def private_lane(*)
end

def unlock_keychain(*)
end

def app_store_connect_api_key(*)
  {}
end

def match(type:, **)
  @matched_type = type
  prefix = "sigh_com.snurhythm.AsoBMaShow_#{type}"
  ENV["#{prefix}_profile-name"] = "Fixture #{type} profile"
  ENV["#{prefix}_team-id"] = "FIXTURETEAM"
  ENV["#{prefix}_certificate-name"] = "Apple Distribution: Fixture"
end

def update_code_signing_settings(**options)
  @signing = options
end

def build_app(**options)
  raise "Archive must use match's installed signing, not automatic development signing" unless @signing
  expected = {
    path: "AsoBMaShow.xcodeproj", targets: ["AsoBMaShow"],
    build_configurations: ["Release"], use_automatic_signing: false,
    profile_name: "Fixture #{@matched_type} profile", team_id: "FIXTURETEAM",
    code_sign_identity: "Apple Distribution: Fixture"
  }
  raise "Signing must only configure the app's Release target: #{@signing}" unless @signing == expected
  raise "Archive configuration must be explicit" unless options[:configuration] == "Release"
  raise "TestFlight clean behavior changed" unless options[:clean] == (@matched_type == "appstore")
  raise "Do not override framework profiles globally" if options[:xcargs].include?("PROVISIONING_PROFILE")
end

def lane_context
  { SharedValues::IPA_OUTPUT_PATH => "/fixture.ipa" }
end

def latest_testflight_build_number(*)
  1
end

def temporary_fix_ios_post_build
end

def firebase_app_distribution(*)
end

def upload_to_testflight
end

load ARGV.fetch(0)

def release_build_identity
  { commit: "a" * 40, source_clean: "1" }
end

def firebase_derived_data_path
  "/fixture-derived-data"
end

def audit_distribution_artifact(*)
end

ENV["GITHUB_ACTIONS"] = "true"
ENV["GITHUB_BASE_REF"] = "develop"
ENV["GITHUB_EVENT_NAME"] = "pull_request"
@lanes.fetch(:firebase).call
@signing = nil
ENV["GITHUB_EVENT_NAME"] = "push"
@lanes.fetch(:testflight_release).call
puts "Both distribution lanes select match's Release signing"
