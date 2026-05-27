# Repository Notes for Agents

## iOS Firebase Deploy

- To deploy an iOS build to Firebase App Distribution from this machine, use:
  `scripts/ios_firebase_deploy.sh`
- Do not run `bundle exec fastlane ios beta` directly unless you have already selected the project Ruby. The default Ruby in this environment is the macOS system Ruby, which is too old for this repo.
- The deploy script loads `.env`, `.env.local`, `ios/Xcode/AsoBMaShow/.env`, and `ios/Xcode/AsoBMaShow/.env.local`, then selects the Ruby version from `ios/Xcode/AsoBMaShow/.ruby-version` or `.tool-versions`.
- Use `scripts/ios_firebase_deploy.env.example` as the private env template. Real env files must stay out of git.
- The script simulates the GitHub PR-to-`develop` environment so the Fastlane `ios beta` lane takes the Firebase App Distribution path instead of TestFlight.
- Running the deploy script uploads a build. Only run it when the user explicitly asks for deployment.

## iOS Build Setup

- `scripts/ios_init.sh` configures the generated bgfx Xcode project and caches only Bundler gems and CocoaPods. It intentionally does not cache `bgfx/build`.
- TestFlight clean builds are intentional. Do not remove the TestFlight clean behavior without the user asking.
- Firebase PR builds should keep the fixed DerivedData path so Xcode can reuse `ArchiveIntermediates`.
