"""Selectors and endpoints observed against the live portal on 2026-09-25.

Login markup comes from eu-west-2.signin.aws /assets/js/app.js. Account and
role markup comes from the signed-in access portal DOM. Credential fetches use
the portal API the page itself calls, not the Access keys modal.
"""

DIRECTORY_ID = "d-9c677042e2"
PORTAL_ORIGIN = "https://d-9c677042e2.awsapps.com"
PORTAL_START = "https://d-9c677042e2.awsapps.com/start/#/?tab=accounts"
PORTAL_API = "https://portal.sso.eu-west-2.amazonaws.com"
SIGNIN_HOST = "eu-west-2.signin.aws"
DEFAULT_REGION = "eu-west-2"
PRODUCTION_ACCOUNT_ID = "382373577178"

# Browser cookie that the portal JS sends as the bearer token.
AUTH_COOKIE_NAMES = (
    "x-amz-sso_authn",
    "__Host-idc-access-portal-authn",
)

USERNAME_INPUT = "#username-input"
USERNAME_SUBMIT = "#username-submit-button"
PASSWORD_INPUT = '[data-testid="test-password-input"]'
PASSWORD_SUBMIT = "#password-submit-button"
MFA_INPUT = '[data-testid="vmfa-authentication"] [data-testid="test-input"]'
MFA_SUBMIT = (
    '[data-testid="vmfa-authentication"] [data-testid="test-primary-button"]'
)
ALERT = '[data-testid="test-alert"]'
ACCOUNTS_TAB = '[data-testid="accounts"]'

# Present on the accounts table. Used only to recognise a signed-in portal.
ACCOUNT_CELL = '[data-testid="account-list-cell"]'
ROLE_LINK = '[data-testid="federation-link"]'
ACCESS_KEYS = '[data-testid="role-creation-action-button"]'
