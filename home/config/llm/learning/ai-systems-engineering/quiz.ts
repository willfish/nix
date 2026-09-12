export type Band = 'good' | 'warn' | 'bad';
export type Grade = { band: Band; text: string };

export type LessonDraft = {
  field: string;
  needles: string[];
  total: number;
  goodAt: number;
  warnAt: number;
  messages: Record<Band, (hits: number, total: number) => string>;
};

export type LessonQuiz = {
  name: string;
  correct: string;
  empty: string;
  good: string;
  bad: string;
};

export type Lesson = {
  quiz?: LessonQuiz;
  template?: string;
  draft: LessonDraft;
};

export function gradeChoice(value: string | undefined, quiz: LessonQuiz): Grade {
  if (!value) return { band: 'warn', text: quiz.empty };
  if (value === quiz.correct) return { band: 'good', text: quiz.good };
  return { band: 'bad', text: quiz.bad };
}

export function countHits(value: string, needles: string[]): number {
  const haystack = value.toLowerCase();
  return needles.filter((needle) => haystack.includes(needle)).length;
}

export function gradeDraft(value: string, spec: LessonDraft): Grade {
  const hits = countHits(value, spec.needles);
  const band: Band = hits >= spec.goodAt ? 'good' : hits >= spec.warnAt ? 'warn' : 'bad';
  return { band, text: spec.messages[band](hits, spec.total) };
}

export const LESSONS: Record<string, Lesson> = {
  '0001': {
    quiz: {
      name: 'q1',
      correct: 'b',
      empty: 'Choose an answer first.',
      good: 'Correct. It names the workflow, tool boundary, state, human review, evals, observability, and fallback behavior.',
      bad: 'Not quite. That answer describes useful AI feature work, but it does not show enough production ownership.',
    },
    template: `System:
User outcome:
Tools:
State and memory:
Eval cases:
Guardrails:
Human review:
Observability:
Cost and latency budget:
Fallbacks:
Portfolio proof:`,
    draft: {
      field: 'onepager',
      needles: ['tool', 'state', 'eval', 'guardrail', 'human', 'observ', 'latency', 'fallback', 'portfolio'],
      total: 9,
      goodAt: 7,
      warnAt: 4,
      messages: {
        good: (hits, total) => `Phase 3 shape: ${hits}/${total} concerns are present. Now make each line concrete enough that another engineer could challenge it.`,
        warn: (hits, total) => `Bridge to Phase 3: ${hits}/${total} concerns are present. Add the missing operational pieces before treating this as a systems design.`,
        bad: (hits, total) => `Still mostly feature-shaped: ${hits}/${total} concerns are present. Start by adding evals, observability, human review, and fallbacks.`,
      },
    },
  },
  '0002': {
    draft: {
      field: 'draft',
      needles: ['node', 'state', 'tool', 'interrupt', 'resume', 'failure', 'approval', 'output'],
      total: 8,
      goodAt: 7,
      warnAt: 4,
      messages: {
        good: (hits, total) => `${hits}/${total} workflow concerns present. Now make each line specific enough to implement.`,
        warn: (hits, total) => `${hits}/${total} workflow concerns present. Add the missing production behavior before coding.`,
        bad: (hits, total) => `${hits}/${total} workflow concerns present. This still reads like an idea, not a workflow spec.`,
      },
    },
  },
  '0003': {
    draft: {
      field: 'draft',
      needles: ['schema', 'output', 'auth', 'trace', 'approval', 'failure', 'mcp', 'effect'],
      total: 8,
      goodAt: 7,
      warnAt: 4,
      messages: {
        good: (hits, total) => `${hits}/${total} boundary concerns present. Next, write evals that prove the model uses these tools correctly.`,
        warn: (hits, total) => `${hits}/${total} boundary concerns present. Add stricter schemas, risk handling, and trace fields.`,
        bad: (hits, total) => `${hits}/${total} boundary concerns present. This is still too loose to operate safely.`,
      },
    },
  },
  '0004': {
    draft: {
      field: 'draft',
      needles: ['input', 'expected', 'tool', 'guardrail', 'grader', 'golden', 'edge', 'adversarial', 'fallback', 'human'],
      total: 10,
      goodAt: 8,
      warnAt: 5,
      messages: {
        good: (hits, total) => `${hits}/${total} eval concerns present. Now turn these into runnable checks or a table in your project README.`,
        warn: (hits, total) => `${hits}/${total} eval concerns present. Add adversarial, human review, and fallback cases.`,
        bad: (hits, total) => `${hits}/${total} eval concerns present. This is not enough evidence to market production reliability.`,
      },
    },
  },
  '0005': {
    draft: {
      field: 'draft',
      needles: ['span', 'metric', 'latency', 'cost', 'token', 'error', 'fallback', 'budget', 'review', 'confidence'],
      total: 10,
      goodAt: 8,
      warnAt: 5,
      messages: {
        good: (hits, total) => `${hits}/${total} ops concerns present. This is ready to become a README operations section.`,
        warn: (hits, total) => `${hits}/${total} ops concerns present. Add budget thresholds and failure-specific fallbacks.`,
        bad: (hits, total) => `${hits}/${total} ops concerns present. The system is not yet observable enough to own.`,
      },
    },
  },
  '0006': {
    draft: {
      field: 'draft',
      needles: ['problem', 'architecture', 'workflow', 'tools', 'state', 'eval', 'guardrail', 'human', 'fallback', 'trace', 'metric', 'cost', 'tradeoff', 'cv'],
      total: 14,
      goodAt: 11,
      warnAt: 7,
      messages: {
        good: (hits, total) => `${hits}/${total} proof elements present. This can become a credible public case study after you replace placeholders with implementation evidence.`,
        warn: (hits, total) => `${hits}/${total} proof elements present. Add operational evidence and tradeoffs before publishing.`,
        bad: (hits, total) => `${hits}/${total} proof elements present. This still reads like a claim rather than evidence.`,
      },
    },
  },
};

type Classy = { className: string; textContent: string; classList: { add: (name: string) => void } };

export function applyGrade(target: Classy, grade: Grade): void {
  target.className = 'result';
  target.textContent = grade.text;
  target.classList.add(grade.band);
}

export function bindLesson(
  lesson: Lesson,
  doc: {
    querySelector: (selector: string) => { value?: string } | null;
    getElementById: (id: string) => (Classy & {
      value?: string;
      addEventListener?: (name: string, handler: () => void) => void;
    }) | null;
  },
): void {
  const score = doc.getElementById('score');
  const draft = doc.getElementById(lesson.draft.field);
  const draftResult = doc.getElementById(lesson.draft.field === 'onepager' ? 'score-result' : 'result');
  score?.addEventListener?.('click', () => {
    if (!draft || !draftResult) return;
    applyGrade(draftResult, gradeDraft(String(draft.value ?? ''), lesson.draft));
  });
  if (lesson.quiz) {
    const quiz = lesson.quiz;
    doc.getElementById('check-quiz')?.addEventListener?.('click', () => {
      const result = doc.getElementById('quiz-result');
      if (!result) return;
      applyGrade(result, gradeChoice(doc.querySelector(`input[name='${quiz.name}']:checked`)?.value, quiz));
    });
  }
  if (lesson.template) {
    const template = lesson.template;
    doc.getElementById('reset')?.addEventListener?.('click', () => {
      if (draft) draft.value = template;
      const result = doc.getElementById('score-result');
      if (!result) return;
      result.className = 'result';
      result.textContent = 'Template reset. Fill in the draft, then score it.';
    });
  }
}

export function lessonIdFrom(path: string, explicit?: string): string | undefined {
  if (explicit && LESSONS[explicit]) return explicit;
  const match = path.match(/(\d{4})-[^/]+\.html$/);
  return match && LESSONS[match[1]] ? match[1] : undefined;
}

const browserDoc = (globalThis as { document?: { documentElement?: { dataset?: { lesson?: string } }; location?: { pathname?: string } } }).document;
if (browserDoc) {
  const id = lessonIdFrom(browserDoc.location?.pathname ?? '', browserDoc.documentElement?.dataset?.lesson);
  if (id) bindLesson(LESSONS[id], browserDoc as Parameters<typeof bindLesson>[1]);
}
