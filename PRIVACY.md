# Privacy Policy

최종 수정일: 2026년 2월 9일 / Last Updated: February 9, 2026

## 1. 개요 / Introduction

Sorana Flow("본 소프트웨어")의 개발자(이하 "개발자")는 이용자의 개인정보를 소중히 여기며, 「개인정보 보호법」(대한민국), EU 일반 데이터 보호 규정(GDPR), 및 캘리포니아 온라인 프라이버시 보호법(CalOPPA)을 준수합니다.

본 개인정보처리방침은 soranaflow.com("웹사이트")과 Sorana Flow 데스크톱 애플리케이션("앱")에 적용됩니다.

The developer of Sorana Flow ("Software") respects your privacy and complies with the Personal Information Protection Act (PIPA) of the Republic of Korea, the EU General Data Protection Regulation (GDPR), and the California Online Privacy Protection Act (CalOPPA).

This Privacy Policy applies to the website soranaflow.com ("Website") and the Sorana Flow desktop application ("App").

## 2. 개인정보 보호책임자 / Data Controller & Chief Privacy Officer

- 개인정보 보호책임자 (CPO): 최해성
- 이메일: haruki7423@gmail.com
- 소재지: 대한민국

- Data Controller & CPO: Haeseong Choi
- Email: haruki7423@gmail.com
- Location: Republic of Korea

## 3. 앱에서 수집하지 않는 정보 / What the App Does NOT Collect

**Sorana Flow 앱은 다음을 수집, 전송, 저장하지 않습니다:**

- 원격 측정(텔레메트리) 또는 사용 분석
- 충돌 보고서
- 사용자 행동 분석
- 개인정보 (이름, 이메일, 전화번호 등)
- 계정 생성이나 로그인 불요

모든 오디오 처리는 사용자의 기기에서 로컬로 수행됩니다.

**The Sorana Flow app does NOT collect, transmit, or store:**

- Telemetry or usage analytics
- Crash reports
- User behavior analytics
- Personal information (name, email, phone, etc.)
- No account creation or login required

All audio processing is performed locally on your device.

## 4. 로컬 데이터 저장 / Local Data Storage

앱은 다음 데이터를 사용자 기기에 로컬로 저장합니다:

- 음악 라이브러리 데이터베이스 (트랙 메타데이터, 재생 목록, 설정)
- 저장 위치: ~/Library/Application Support/SoranaFlow/

이 데이터는 개발자의 서버로 전송되지 않으며, 어떠한 제3자에게도 전달되지 않습니다. 사용자는 해당 폴더를 삭제하여 언제든지 이 데이터를 완전히 삭제할 수 있습니다.

The App stores the following data locally on your device:

- Music library database (track metadata, playlists, settings)
- Location: ~/Library/Application Support/SoranaFlow/

This data is never transmitted to the developer's servers or any third party. You may delete this data at any time by removing the above folder.

## 5. 앱의 제3자 서비스 연동 / Third-Party Services Used by the App

앱은 사용자의 기기에서 직접 다음 제3자 서비스에 연결합니다. 개발자는 이 통신을 중개, 가로채기, 저장하지 않습니다. 표준 네트워크 요청의 일부로 사용자의 IP 주소가 해당 서비스에 노출될 수 있습니다.

The App connects directly from your device to the following third-party services. The developer does not proxy, intercept, or store any data exchanged. Your IP address may be visible to these services as part of standard network requests.

### a) Apple Music (선택 사항 / Optional)

사용자가 macOS 시스템 프롬프트를 통해 명시적으로 승인한 경우에만 활성화됩니다. Apple의 MusicKit 프레임워크를 통해 Apple 서버와 직접 통신합니다. Sorana Flow는 Apple Music 인증 정보를 수신하거나 저장하지 않습니다. 처리 근거: 이용자 동의 (명시적 옵트인).

Activated only when the user explicitly authorizes via macOS system prompt. Communicates directly with Apple's servers via MusicKit. Sorana Flow does not receive or store Apple Music credentials. Legal basis: User consent (explicit opt-in).

[Apple Privacy Policy](https://www.apple.com/legal/privacy/)

### b) MusicBrainz / AcoustID (트랙 식별 / Track Identification)

오디오 핑거프린트(약 2.5KB의 수학적 표현, 오디오로 복원 불가, 개인 식별 불가)와 트랙 길이를 AcoustID에 전송합니다. 반환된 녹음 ID를 사용하여 MusicBrainz에서 메타데이터를 조회합니다. 처리 근거: 정당한 이익 (핵심 기능).

Sends a Chromaprint audio fingerprint (~2.5KB mathematical representation — not reconstructable into audio, not personally identifiable) plus track duration to AcoustID. Returned recording IDs are used to fetch metadata from MusicBrainz. Legal basis: Legitimate interest (core functionality).

[MusicBrainz Privacy Policy](https://musicbrainz.org/doc/MusicBrainz_Privacy_Policy)

### c) Fanart.tv / Cover Art Archive (아트워크 / Artwork)

MusicBrainz 아티스트/앨범 식별자만 전송합니다. 개인 데이터는 전송되지 않습니다. 처리 근거: 정당한 이익.

Sends MusicBrainz artist/album identifiers only. No personal data transmitted. Legal basis: Legitimate interest.

### d) LRCLIB (가사 / Lyrics)

트랙 제목, 아티스트명, 앨범명을 전송합니다. 표준 HTTP 요청 메타데이터 외의 개인 데이터는 전송되지 않습니다. 처리 근거: 정당한 이익.

Sends track title, artist name, album name. No personal data beyond standard HTTP request metadata. Legal basis: Legitimate interest.

### e) Sparkle (업데이트 확인 / Update Checks)

주기적으로 soranaflow.com/appcast.xml에 연결합니다. 표준 HTTP User-Agent 헤더를 통해 앱 버전과 macOS 버전만 전송합니다. 사용자는 설정에서 자동 확인을 비활성화할 수 있습니다. 처리 근거: 정당한 이익 (보안/유지보수).

Periodically connects to soranaflow.com/appcast.xml. Transmits only app version and macOS version via HTTP User-Agent header. Users can disable automatic checks in preferences. Legal basis: Legitimate interest (security/maintenance).

### f) Ko-fi (기부 / Donations)

기부 링크를 클릭하면 ko-fi.com으로 이동합니다. 개발자는 결제 정보를 수신하지 않습니다.

Clicking the donation link redirects to ko-fi.com. The developer does not receive payment information.

[Ko-fi Privacy Policy](https://more.ko-fi.com/privacy)

## 6. 웹사이트 데이터 수집 / Website Data Collection

### 6.1 Google Analytics (GA4)

웹사이트는 익명의 방문자 통계를 위해 Google Analytics를 사용합니다. GA4는 기본적으로 IP 익명화가 활성화되어 있습니다. 데이터는 Google에 의해 처리되며 EU-US 데이터 프라이버시 프레임워크에 따라 미국으로 이전될 수 있습니다. 처리 근거: 이용자 동의.

The Website uses Google Analytics for anonymous audience measurement. IP anonymization is enabled by default. Data is processed by Google and may be transferred to the United States under the EU-US Data Privacy Framework. Legal basis: Consent.

| Cookie | Provider | Category | Purpose | Duration |
|--------|----------|----------|---------|----------|
| `_ga` | Google | Analytics | 고유 방문자 구분 / Distinguish visitors | ~13 months |
| `_ga_<ID>` | Google | Analytics | 세션 상태 유지 / Session state | ~13 months |

[Google Privacy Policy](https://policies.google.com/privacy) · [Google Analytics Opt-out](https://tools.google.com/dlpage/gaoptout)

### 6.2 Cloudflare (보안 / Security)

| Cookie | Provider | Category | Purpose | Duration |
|--------|----------|----------|---------|----------|
| `__cf_bm` | Cloudflare | Strictly Necessary | 봇 탐지/보안 / Bot detection | 30 min |
| `cf_clearance` | Cloudflare | Strictly Necessary | 보안 인증 / Security clearance | Variable |

처리 근거: 정당한 이익 (보안). 필수 쿠키입니다.

Legal basis: Legitimate interest (security). These are strictly necessary cookies.

[Cloudflare Privacy Policy](https://www.cloudflare.com/privacypolicy/)

### 6.3 Netlify (호스팅 / Hosting)

서버 접근 로그(IP 주소, 브라우저 유형, 방문 페이지, 타임스탬프)를 수집합니다. 로그는 최대 30일간 보관됩니다. 쿠키를 설정하지 않습니다. 처리 근거: 정당한 이익.

Collects server access logs (IP address, browser type, pages visited, timestamps). Retained up to 30 days. No cookies. Legal basis: Legitimate interest.

[Netlify Privacy Policy](https://www.netlify.com/privacy/)

## 7. 쿠키 관리 / Managing Cookies

브라우저 설정을 통해 쿠키를 비활성화할 수 있습니다. Google Analytics 옵트아웃 브라우저 애드온을 설치하거나 광고 차단기를 사용할 수도 있습니다. 필수 보안 쿠키(Cloudflare)는 웹사이트 정상 작동에 필요합니다.

You may disable cookies via browser settings. You may also install the Google Analytics Opt-out Browser Add-on or use an ad blocker. Strictly necessary security cookies (Cloudflare) are required for normal website operation.

## 8. 국외 이전 / International Data Transfers

웹사이트 데이터는 Google, Cloudflare, Netlify에 의해 미국에서 처리될 수 있습니다. 이전 근거: EU-US 데이터 프라이버시 프레임워크 (미국 서비스), 대한민국-EU 적정성 결정 (2021년 12월 17일), 개인정보 보호법 개정안에 따른 해외 이전 규정. 앱의 제3자 API 요청은 각 서비스의 서버 소재지로 전송됩니다.

Website data may be processed in the United States by Google, Cloudflare, and Netlify. Transfer mechanisms: EU-US Data Privacy Framework for US services; South Korea-EU adequacy decision (December 17, 2021). App API requests reach services in their respective jurisdictions.

## 9. 이용자의 권리 / Your Rights

관할권에 따라 다음 권리를 행사할 수 있습니다:

- 개인정보 열람, 정정, 삭제 요청권
- 처리 제한 요청권 및 처리 거부권
- 데이터 이동권
- 동의 철회권
- 자동화된 의사결정에 대한 거부권 (개인정보 보호법 2023년 개정)
- 감독 기관에 이의 제기권 (대한민국: 개인정보보호위원회, EU: 해당 감독기관)

앱은 개인정보를 수집하지 않으므로, 대부분의 권리는 웹사이트 데이터(Google Analytics 및 서버 로그)에만 적용됩니다.

Depending on your jurisdiction, you may exercise: right to access, rectification, erasure, restriction of processing, data portability, objection to processing, withdrawal of consent, right to refuse automated decisions (PIPA 2023 amendment), and right to lodge a complaint with a supervisory authority (PIPC in Korea; relevant EU DPA for EU residents).

Since the App collects no personal data, most rights apply only to website data.

권리 행사 연락처 / Contact: haruki7423@gmail.com

## 10. 아동 보호 / Children's Privacy

본 서비스는 만 14세 미만의 아동을 대상으로 하지 않습니다. 개발자는 만 14세 미만 아동의 개인정보를 고의로 수집하지 않습니다. 만 14세 미만 아동이 개인정보를 제공한 것으로 판단되는 경우 즉시 삭제 조치합니다.

The Service is not directed at children under 14. We do not knowingly collect personal information from children under 14.

## 11. 보안 조치 / Security Measures

웹사이트: HTTPS 암호화 (Netlify/Cloudflare), DDoS 방어 (Cloudflare). 앱: 로컬 데이터는 macOS 표준 Application Support 디렉토리에 사용자의 파일 시스템 권한으로 저장됩니다. 개발자의 서버에 개인정보가 저장되지 않습니다.

Website: HTTPS encryption, DDoS protection via Cloudflare. App: Local data stored in macOS Application Support directory with native filesystem permissions. No personal data stored on the developer's servers.

## 12. 방침 변경 / Changes to This Policy

본 방침이 변경될 경우 웹사이트에 변경 사항과 새로운 시행일을 게시합니다. 중요한 변경 사항은 시행일 최소 7일 전에 공지합니다. 서비스를 계속 이용하면 변경된 방침에 동의하는 것으로 간주됩니다.

Changes will be posted with a new effective date. Material changes will be announced at least 7 days before taking effect. Continued use constitutes acceptance.

## 13. 연락처 / Contact

- 이메일 / Email: haruki7423@gmail.com
- 웹사이트 / Website: https://soranaflow.com
