## ADDED Requirements

### Requirement: A kept file is converted only by choice
A file that Axis keeps (a file kept by hand, or one brought over from Radarr or Sonarr) SHALL NOT be replaced by a conversion unless an administrator chose to convert kept files for that conversion: by a box of its own on the confirmation of a single replacement, not ticked by default, or through a conversion run that has the choice. The choice SHALL be recorded with the conversion, with who asked, and SHALL be what the condition is worked out with at the request, when a worker takes the work and immediately before placement. The choice SHALL lift that one condition and no other: every other condition and all ten checks SHALL apply to a kept file as to any file. A conversion SHALL NOT change whether a file is kept: after a replacement the converted file SHALL be kept exactly as its original was, in the same link, and everything else that reads the mark SHALL behave as before. A trial replaces nothing, so the condition SHALL NOT apply to it.

#### Scenario: Refused without the choice
- **WHEN** a replacement of a kept file is requested without the choice
- **THEN** it is refused, saying that Axis keeps the file, and nothing is queued

#### Scenario: Converted with the choice
- **WHEN** an administrator requests a replacement of a kept file with the choice ticked, and every other condition holds
- **THEN** the conversion is queued and records the choice, passes the same ten checks, takes its original's place, and the file is still kept afterwards

#### Scenario: Another condition still refuses
- **WHEN** a replacement of a kept file is requested with the choice and the profile version is not proven
- **THEN** it is refused for that condition

#### Scenario: Kept while in flight, without the choice
- **WHEN** a file becomes kept after a replacement of it was requested without the choice
- **THEN** the conversion is rejected before placement, its output is removed and the original is unchanged

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator, or a request without the CSRF token, asks for a conversion with the choice
- **THEN** the response is 403 and nothing is queued

#### Scenario: Offered only where it decides
- **WHEN** the confirmation of a replacement is shown for a kept file that no other condition holds back
- **THEN** it offers the choice as a box that is not ticked, with a sentence saying that the file is re-encoded in place after every check passes and stays kept

### Requirement: Progress of a conversion in flight
A conversion that is in flight SHALL be shown with a progress bar (the HTML `progress` element) on its own page and in the list of the latest conversions, with the same words beside the bar and as the bar's accessible name. The words SHALL name the stage (waiting for a worker, encoding, checking, replacing, or keeping a trial aside) and, once the worker has reported progress, the percent; and, when they can be worked out from what the worker reports and the time the work has run, the speed as a multiple of real time and an estimate of the time left. The bar SHALL carry the percent as its value out of 100, and SHALL carry no value (an indeterminate bar) while nothing is known of how far the work is. The page and the list SHALL follow a conversion in flight without a reload, and SHALL stop asking once nothing is in flight. These fragments SHALL be an administrator's only. The worker protocol SHALL NOT change for this.

#### Scenario: No progress yet
- **WHEN** a conversion waits for a worker, or a worker has taken it and reported nothing yet
- **THEN** its bar has no value and the words say the stage, and for an encode that no progress is reported yet

#### Scenario: At the start, in the middle and at the end
- **WHEN** the worker has reported 0, 50 and 100 percent of the source read
- **THEN** the bar's value is 0, 50 and 100 of 100, and the words say the percent

#### Scenario: Speed and time left
- **WHEN** an encode of a ten-minute source has run for two minutes and is half done
- **THEN** the words say 2.5 times real time and about 2 minutes left

#### Scenario: Ended
- **WHEN** the block that follows a conversion asks again after the conversion ended
- **THEN** what comes back has no bar, asks for nothing more, and points at the conversion's page
